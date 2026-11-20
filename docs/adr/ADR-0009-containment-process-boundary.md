<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0009: Containment -- a process boundary at the dma-buf seam; seccomp alone rejected

- **Status:** Accepted (boundary decided; the process split runs on x86-64 as `--split`, with
  the child confined by namespaces and a seccomp allowlist and the broker validating what it
  sends; native GL and video cross; nothing here has had outside review, so untrusted APKs
  are still not a supported use). Refines ADR-0003.
- **Date:** 2026-11-13
- **Provenance:** plan.md §4 Phase 5, §6 R-10; docs/ndk-scope.md (trust model);
  docs/split-process-art.md; ADR-0001, ADR-0003, ADR-0005; `src/ipc/`, `src/runtime/split_vm.cpp`
  as they stand at this date. Kernel interfaces as documented in seccomp(2), landlock(7) and
  user_namespaces(7); bubblewrap as documented in bwrap(1). No GPL-3.0 sources.

## Context
R-10: an app's native code is arbitrary code in a4l's single glibc process. It reaches ART, Mesa
and the Wayland connection, with no per-app UID, SELinux domain or seccomp filter. R-10 named the
mitigation as "a seccomp filter around the bionic-namespace threads" and deferred it to this ADR.

That mitigation does not hold, for four reasons that are properties of the mechanism, not of a4l:

- **A seccomp filter attaches to a thread, not to a library.** There is no thread that runs only
  app code. A `RegisterNatives`'d app native is invoked on an ART thread through the `%fs`
  trampoline (docs/ndk-scope.md, rung 4b onward), and the same thread runs a4l's own JNI natives --
  the ones that issue DRM ioctls and write the Wayland socket. A filter tight enough to constrain
  the app denies a4l; a filter loose enough for a4l permits the app.
- **File descriptors are per-process.** The Wayland socket, the DRM fds and the PipeWire
  connection are open in the app's fd table. Any filter that lets a4l `sendmsg` on them lets the
  app do the same.
- **Memory is shared.** App code can call a4l and ART functions directly and overwrite their
  data. seccomp narrows syscalls; it says nothing about which code issued one. Filtering on the
  syscall's instruction pointer does not rescue this: the app can jump to a `syscall` instruction
  inside an allowed range.
- **A filter cannot be loosened once installed**, so it cannot be scoped to the duration of an
  app call on a shared thread.

So a filter on the shared process is syscall narrowing, not confinement. Confinement needs an
address space, an fd table and a credential that the app does not share with the platform side.

A second fact, from an unrelated decision: that boundary is already drawn. On arm64 a glibc
process cannot `dlopen` the bionic `libart.so`, so `Backend::ArtSplit` runs ART, the framework and
the render half in `acl-art-host` and leaves present, input, audio and `pkg/` in the a4l process
(docs/split-process-art.md). What crosses is 12 fixed message tags over one `SOCK_SEQPACKET` pair,
with dma-buf, fence and shm fds as `SCM_RIGHTS` (`src/ipc/ipc.h`). It was built for an ABI reason.
It is also where Android puts the same line: the app renders in its own process with the GPU
driver loaded, and the compositor is elsewhere.

State of that path, from `git log` rather than from the doc: S1 (ART boots in the subprocess,
x86-64 and a Pi 4) and the S2 present-sink seam landed 2026-07-15/16; nothing since on arm64.
On x86-64 the split runs as of 2026-11-14 (`a4l run --split`, see C2 below): frames and input
cross, for Skia and for native GL, and decoded video crosses to the video plane. Audio crosses
as of 2026-11-15 (see "S4 as built: audio" below). The child is confined as of 2026-11-14
(C4).

## Decision
1. **The containment boundary is a process boundary at the dma-buf seam.** Everything an APK
   supplies -- DEX, native libraries, assets -- executes in a child that holds no display, audio
   or session connection. The a4l process is the broker: it owns the Wayland connection or DRM
   master, PipeWire, the portals, and `pkg/`.
2. **seccomp is applied to that child, and is not claimed as containment anywhere else.** The
   child is confined before it executes any byte from the APK: `no_new_privs`, a seccomp-bpf
   allowlist, and either namespaces (user, mount, pid, net, ipc) or Landlock for the filesystem.
   It inherits the IPC socket and a GPU render node and nothing else.
3. **The broker treats every message as hostile.** Each field is validated before it reaches a
   compositor or KMS import.
4. **Integrity stays where R-10 put it, and runs in the broker.** One APK v2/v3 verification in
   `pkg/` when the APK is opened, before the child is spawned.
5. **`Backend::ArtSplit` is the containment seam and is not retired by B2.** A single-libc arm64
   ART would remove the ABI reason for the split; it does not remove this one. The split becomes
   a supported configuration on x86-64 as well.
6. **Single-process remains, for vetted APKs only.** It is the development path and the right
   configuration where the integrator owns or audits the app. It is not a supported way to run an
   untrusted APK, and no hardening of it changes that.

## Consequences
**Gate before "runs untrusted APKs" is claimed.** All five, on the architecture claimed:

| | Condition | Today |
|---|---|---|
| C1 | APK v2/v3 verification in `pkg/`, in the broker | met, 2026-11-14: `--verify=require` or `--signer` runs a private copy that is verified on every run (`pkg/apk_verify.cpp`, `pkg/apk_stage.cpp`). Under `--split` the broker does this before it starts the child, and the child is handed the copy. ART's compiled-code cache is trusted by location, not verified |
| C2 | Split path at S5: a real APK renders and takes input | x86-64, Skia path, 2026-11-14: `a4l run --split` runs the app in a child a4l process whose presenter is a proxy (`wm/split_present.cpp`); the broker holds the only compositor connection. 3 corpus apps render on host ART; one presents 1176 frames in 20 s split against 1178 single-process. Input is forwarded and unit-tested; there is no controlled live input test. Not done: arm64 (`acl-art-host` is still S1), audio (a confined child cannot reach PipeWire and is silent; unconfined it opens PipeWire itself) |
| C3 | Native GL and video cross the boundary as dma-buf | native GL: x86-64, 2026-11-14. In a `--split` child the app's GL window is a gbm surface and each swapped buffer goes to the broker as a `PresentFrame`, like a Skia frame. 2 libGDX apps and 4 Flutter example apps render confined. See "C3 as built" below. Video: x86-64, 2026-11-14. Each frame a `MediaCodec` decodes for a video `SurfaceView` goes to the broker as a `VideoFrame` with its own dma-buf, and the broker puts it on the subsurface below the window. One fixture app, H.264 through VA-API: 180 of 180 frames crossed, confined. See "C3 as built: video" below |
| C4 | Child confined per Decision 2 | x86-64, 2026-11-14: `--split` starts the child through bubblewrap (`src/confine/`) in its own user, mount, pid, ipc, uts and network namespaces, and the child installs a seccomp allowlist on itself before it opens the APK. See "C4 as built" below for what it can reach and what is not covered. arm64: none |
| C5 | Broker validates every message; receive path fuzzed | x86-64 `--split` broker, 2026-11-14: every frame is checked against what the kernel says about its descriptor, the surface and the compositor's advertised modifiers before it reaches the presenter (`wm/split_validate.cpp`), and the receive path is fuzzed (`tests/fuzz/broker_fuzz.cpp`). See "C5 as built" below. Not covered: the arm64 `acl-art-host` receive path (`runtime/split_vm.cpp`), and what the compositor does with a buffer that passes |

**What it costs.**
- **The native-GL path changes shape.** Single-process, Mesa commits a4l's own `wl_surface`
  inside `eglSwapBuffers` and a4l allocates no buffer. A confined child has no `wl_surface`. It
  must render to an exported dma-buf and send `PresentFrame`. Still no CPU copy; the "allocates no
  buffer" property is a single-process property. This is the largest piece of C3.
- **Datagrams per frame.** Measured 2026-11-14 over 615 frames: 2.0 from the child (the frame
  with its dma-buf fd, and the keepalive the deferred render path sends) and 1.0 to it (the
  compositor's frame callback). No fd goes back; there is no release fence yet. Frame count and
  time to first frame match single-process within run-to-run noise (504 ms single, 538 ms split,
  one run each).
- **The GPU driver stays reachable from the child.** Render-node ioctls are a large kernel
  surface, and the filter cannot remove them without removing rendering. Android accepts the same
  exposure. Video decode stays in the child for the same reason it renders there: it parses data
  the app supplies.
- **On x86-64 the child is today's whole process minus present, input and audio**: glibc host ART
  plus the bionic bridge of ADR-0005. As built, it is the a4l binary itself, started by the broker
  with the socket in `ACL_SPLIT_FD`. On arm64 it is all-bionic and needs no `%fs` bridge.

**What it does not fix.** A second process under the same UID is not a boundary by itself:
unconfined (`--split=unconfined`) the child can open the user's files and, where Yama permits,
reach the broker through `ptrace` or `/proc/<pid>/mem`. Kernel and GPU-driver bugs, side channels
and resource exhaustion are outside what this decides; a memory and CPU bound on the child is
follow-on work.

**C4 as built (x86-64, 2026-11-14).** `a4l run --split` confines the child; `--split=unconfined`
is the earlier behavior and has to be asked for. If the host cannot confine (no bubblewrap, user
namespaces disabled, an architecture with no system call list) the run is refused with exit 2
and no child is started.

- **Namespaces, by bubblewrap.** User, mount, pid, ipc, uts, cgroup where available, and
  network. `no_new_privs`, no capabilities, no nested user namespace, a new session, and the
  child dies with the broker.
- **Filesystem.** A new root holding, read-only: `/usr`; a list of 21 files and directories
  under `/etc` (loader, resolver, TLS roots, fontconfig, GPU loader configuration); the APK; the
  a4l binary; and the paths a4l itself was told to read through its own options and environment
  (the ART home, the boot classpath, the bionic runtime). Writable: the app's data and cache
  directories (`$XDG_DATA_HOME/acl/<package>`, `$XDG_CACHE_HOME/acl/<package>`) and a private
  256 MiB `/tmp`. Not present: the home directory, `/run` and with it the compositor, PipeWire
  and D-Bus sockets, other apps' directories, other apps' verified copies. An input that would
  put the home or runtime directory in view is refused, not bound.
- **Devices.** `/dev/null` and its siblings, and the DRM render nodes with their sysfs device
  directories. No `card*` node, so no modesetting.
- **Environment.** An allowlist: a4l's own variables, the loader's, locale, GPU driver
  selection. `WAYLAND_DISPLAY`, the D-Bus address, agent sockets and anything unrecognized are
  not passed.
- **Descriptors.** Standard output and error, `/dev/null` as input, and the IPC socket.
- **seccomp.** An allowlist of 175 system calls installed by the child on itself, after a4l has
  started and before it opens the APK; a call off the list answers `ENOSYS`
  (`ACL_SPLIT_SECCOMP=kill` kills instead). 98 were recorded from nine APKs, the five of the
  corpus and four Flutter examples (`tools/record-seccomp.py`); 77 were added in review as
  fallbacks and siblings of recorded
  calls. Three calls are checked by argument: `clone` may not create a namespace, `socket` is
  limited to four address families, `ioctl` may not push terminal input. `execve`, `fork`,
  `ptrace`, `mount`, `unshare`, `bpf`, `io_uring` and the like are absent.
- **Self-check.** The child refuses to continue unless it is in namespaces other than the
  broker's, has `no_new_privs` and the filter, and holds no descriptor beyond the four above.

Measured on host ART under headless Weston, 20 s each: `octalsuntime` 1179 frames confined
against 1178 unconfined, `forkyz` 1169 against 1169, `foss4g` 2 against 2. In 40 s traced runs of
four APKs with the filter on, the only call it denied was `clone3`, once per process, after
which the C library uses `clone`.

Not covered, and what keeps C4 from being the last word:

- **The GPU.** The render node's ioctls are not filtered. This is the exposure named above.
- **The recording is narrow.** Nine APKs, no injected input, no reachable network. A call an app
  makes only on a touch or a completed connection may be off the list; it fails with `ENOSYS`.
  Nobody outside the project has reviewed the list.
- **No system services.** With no D-Bus, connectivity, battery, power, Bluetooth,
  storage and service discovery fall back to their null backends, and the keystore is in memory.
  These belong in the broker, not in the sandbox; they have not moved. Audio has: the broker
  plays the child's streams ("S4 as built: audio" below). The master volume has not, so
  `AudioManager` volume calls do nothing under `--split`.
- **Network is all or nothing.** The default is none. `--net=host` shares the host's network
  namespace, and with it every abstract socket and loopback service, an X server among them.
  A filtered network needs a proxy in the broker or a user-mode network stack.
- **`/usr` and one sysfs device directory are readable.** Host software versions are visible.
- **No resource bound.** No cgroup limit on memory, CPU or process count, and `/dev/shm` is
  not sized.
- **The terminal.** The child writes to the caller's standard output and error. It cannot push
  input to a terminal (`TIOCSTI` and `TIOCLINUX` are refused, and it has its own session), but
  it can print escape sequences and change terminal settings.
- **Landlock is not used.** Decision 2 allows either; this is the namespace form.
- **arm64 (2026-11-18).** Has a system call list of its own, recorded on a Raspberry Pi 5
  from five APKs (`src/confine/seccomp_allow_aarch64.inc`): 88 calls recorded, 10 that
  only x86-64's larger corpus made, 53 added in review. The same day it was recorded again
  on the Pi and on an SA8155P board (vendor kernel, GPU and decoder), 17 runs of eight app
  forms: one call moved to the recorded group and none was outside the list, and every form
  ran on both with the filter in `kill` mode. Its corpus is smaller: no app with
  native code runs on arm64 yet. On the Pi the video fixture plays confined with the filter
  in `kill` mode, HEVC through the board's hardware decoder, and so do a Java-only app and
  the GLSurfaceView fixture.
- **Hardware video decoders (2026-11-18).** A decoder that is its own device (V4L2, on Arm
  boards) is reached by its node, as the GPU is. The sandbox is given each `/dev/video*` that
  says it is a decoder and nothing else (memory to memory, compressed input only) and the
  media controller of the same driver; cameras, encoders and converters in the same directory
  are not passed. It is one more kernel driver the app process can reach. Without it the Pi's
  confined app decoded in software.
- **`/sys/devices/system/cpu`** is visible, read-only (2026-11-18). With it hidden, GStreamer
  scanning its plugins in the app process (its scanner is a program, and the filter refuses
  `execve`) loaded a plugin that aborted for want of it.
- **A GPU and a decoder outside DRM and V4L2 M2M (2026-11-18, an SA8155P board).** Qualcomm's
  downstream Adreno driver has no render node: the sandbox is given `/dev/kgsl-3d0` and, beside
  it only, the ion allocator its user space and the vendor's decoder take every buffer from.
  The decoder (`msm_vidc`) does not report the M2M capability, so a node that has a capture
  and an output queue counts as well; it must still take compressed formats only and give
  none. The vendor's OpenMAX core then decodes confined, with the filter in `kill` mode, and
  so does GStreamer's OpenMAX element, whose configuration (`/etc/xdg/gstomx.conf`) is
  visible. The app process must run as a user that may open these nodes: bubblewrap drops
  every capability, root's included.
- **A card name without a card (2026-11-18, the same board).** That driver's gbm will not
  start without a `/dev/dri/card*` it can open; it opens the first and makes no call on it.
  A card node is the display controller and is not passed. Beside `/dev/kgsl-3d0` the
  sandbox has `/dev/dri/card0` as a link to the render node, which the app has already, so
  nothing is added to what it can reach. With it the GPU video converter and a GLSurfaceView
  come up confined. Two things in a4l went with it, neither a matter of confinement: that
  driver lets a config draw only to a gbm surface of the config's own format, so the window
  is made in the app's config's format and configs a window can be made for are listed
  first; and its surface has three buffers where Mesa's has four, so a swap waits for the
  compositor to let one go before the app draws again.
- **GStreamer's plugin registry is built ahead (2026-11-18).** Before the app process starts,
  the broker runs `a4l gst-registry` in the same sandbox, without the filter, and that
  writes the registry to `<cache>/acl/gst`, which the app process sees read-only. The app
  process is told to use that registry as it is and never to start a scanner
  (`GST_REGISTRY`, `GST_REGISTRY_UPDATE=no`, `GST_REGISTRY_FORK=no`), so it loads the plugins
  it uses and no others. Before this a first run through GStreamer in `kill` mode ended at
  the scanner's `execve`. Cost per launch once the registry exists: 20 ms on a desktop, 30 to
  40 ms on a Pi 5, about 100 ms on the SA8155P board. The first time: 0.3 s, 0.6 to 0.9 s and
  0.45 s.

**C3 as built (x86-64, 2026-11-14): native GL.** Single-process, Mesa's Wayland platform does
three things inside `eglSwapBuffers`: it hands the finished buffer to the compositor, takes back
the buffers the compositor released, and blocks until the compositor wants another frame. A
split child has no compositor connection, so a4l does the same three against the broker.

- **The DRM class entry (2026-11-18).** Each render node has its entry in `/sys/class/drm`,
  a link into the device directory that was visible already. udev lists devices by class,
  GStreamer's VA plugin asks udev for the DRM devices, and with none listed it registered
  no decoder: GStreamer in the app process decoded in software on a machine with VA-API.
  The registry file's name carries a number that goes up with such a change, since
  GStreamer does not rescan a plugin whose file is the same.
- **The window is a gbm surface.** The child's GL device is Mesa's gbm platform on the render
  node. The surface is created with the modifiers the compositor advertised, which the broker
  passed on when the surface was made.
- **Swap exports.** On swap the front buffer is locked and exported as a dma-buf, and sent as
  a `PresentFrame`. It stays locked, so the driver will not draw into it, until the broker
  reports its slot free again (`gfx::GlWindow` hold mode). With no slot free the frame is
  dropped and the frame loop kept alive.
- **Swap paces.** The swapping thread then waits for the broker to report the compositor's
  frame callback, up to 100 ms, as a swap interval of one does.
- **Buffers change under a slot.** An engine recreates its window surface (Flutter does, twice
  at start), and a gbm surface rotates up to four buffers through the presenter's three slots.
  The presenter now rebuilds a slot's `wl_buffer` when the dma-buf in it changes, told apart by
  inode number.
- **Offscreen surfaces.** Mesa's gbm platform has window configs only. An engine that asks for
  a pbuffer config (Impeller's offscreen context) gets a window config, and its pbuffer is a
  gbm surface of that size that is never presented.
- **No stale "free".** `PresentState` now carries the number of frames the broker had handled
  when it wrote the state. A state written before the child's latest frame arrived no longer
  marks that frame's slot free. This closes a window that the Skia path had as well.

The frames are ordinary `PresentFrame` messages and pass the C5 checks unchanged: XRGB8888, an
advertised modifier, two planes on this driver.

Measured on host ART, confined, filter on, 20 s each:

| App | confined | `--split=unconfined` |
|---|---|---|
| `halma` (libGDX), frames | 1216 | 1216 |
| `doodledudette` (libGDX), frames | 1212 | 1212 |
| Flutter `image_list` with `--net=host`, frames in 25 s | 1365 | 1367 |
| Flutter `hello_world`, `layers`, `platform_channel` | 2 each | 2 for `hello_world` |

The three Flutter apps with two frames are static: they draw once per window and stop.
Screenshots from a headless sway show the libGDX menu and Flutter's text, upright and in the
right colors. In 40 s traced runs of five of these apps the filter denied only `clone3`.

Not covered:

- **Video** crosses by its own messages; see the next section.
- **`image_list` without a network** fails in the app, before it draws: it fetches from
  `localhost` by name, and name lookup in an empty network namespace finds no address.
- **Live input** under `--split` is still unverified for both paths.
- **Resize.** The surface keeps the size it was created with.
- **The offscreen substitute leaks** one small gbm surface per pbuffer the engine destroys.
- **One GPU, one driver.** AMD with radeonsi. Not run on Intel or on a driver whose gbm
  surfaces behave differently.
- **Tests.** The gbm hold mode and the state numbering have unit tests. The swap path itself,
  the slot rebuild and the pbuffer substitute are covered only by the runs above; none of them
  runs in CI, which has no GPU.

**C3 as built: video (x86-64, 2026-11-14).** Single-process, a decoded frame for a video
`SurfaceView` does not pass through the app's own buffers. The decoder hands the presenter a
dma-buf, and the presenter attaches it to a subsurface below the window, scaled and cropped to
the View's rectangle by `wp_viewport`; the app draws that rectangle transparent. In a split
child the decoder still runs in the app process (VA-API on the render node, which the sandbox
already binds), and four messages carry the rest to the broker.

- **`VideoFrame`** carries one decoded frame: its dma-buf, size, format, modifier and plane
  layout, and where to show it. It is sent from the decode thread. The broker hands it to the
  real presenter and shows it at once.
- **`VideoBounds`** carries the View's rectangle in the window, sent when it changes.
- **`VideoHide`** and **`VideoReset`** take the picture off the plane, the second when the app
  changes screen.
- **Pacing.** The decoder is already paced to the stream's clock. A broker that falls behind
  makes the child's send block; a compositor that falls behind makes the broker drop the frame
  (at most four are held).

What the broker checks (C5) before a video frame reaches the compositor:

- **The buffer.** The kernel must report the descriptor as a dma-buf, as for a window frame.
- **Format and modifier.** The format is NV12 or P010, or XBGR8888 for a picture the child
  converted (below), and the pair of format and modifier is one the compositor advertised.
  This is stricter than for a window frame: linear is not assumed, because a compositor need
  not take YUV at all.
- **Layout.** NV12 and P010: two to four planes. Each luma row and each chroma row fits its
  stride, and the last row of each plane ends inside the buffer, computed in 64 bits.
  XBGR8888: one to four planes, each row of four-byte pixels fits the stride, and the last row
  ends inside the buffer.
- **Size.** The picture is 1 to 16384 pixels each way. It is the decoder's size, not the
  window's.
- **Rectangles.** A rectangle has an area and lies within 65536 of the window's origin.
- **Placement.** The rectangle is cut to the window and the frame cropped to match
  (`wm/video_geometry.h`). A crop that is empty or reaches outside the frame is a protocol
  error in `wp_viewport`, which would close the broker's connection; the function returns no
  such crop for any input, and a rectangle that shows less than one pixel of the frame shows
  nothing.

A refused video frame is counted by reason and logged once per reason. It does not touch the
window's own frame loop.

Measured on host ART under headless Weston, confined, filter on: the fixture app
(`tests/fixtures/video_plane`) decodes a 640x360 H.264 clip of 60 frames three times onto a
`SurfaceView`. 180 of 180 frames crossed and none was refused; the window itself presented 416
frames in the same run. Single-process, the same app renders 180. Screenshots from a headless
sway, taken at the same point of the clip in both modes, are the same picture: the test
pattern at the View's position, in the right colors, with the app's text drawn over the
window. A traced run made no system call outside the allowlist but `clone3`, which is answered
as before; the list is unchanged.

Tested: unit tests reach every refusal of a video frame and of a rectangle, and run the
placement function over 200000 random inputs against what the protocol accepts. The broker
tests send video from a second thread, as the decoder does, and send each malformed form. The
fuzz harness checks the same properties on whatever reaches the presenter: 29.2 million
executions in five minutes on eight workers, no finding. `tests/integration/run_split_video_test.py`
runs the fixture app end to end and skips without a GPU, so it does not run in CI.

Not covered:

- **One app, one clip, one decoder.** A fixture app, H.264 at 640x360 as NV12, VA-API on AMD.
  No media3 or ExoPlayer app has run under `--split`; none is in the corpus. P010, HEVC and
  VP9 are accepted by the check and have not been sent.
- **Video in a texture.** A Flutter `video_player` clip goes through `ImageReader` into a GL
  texture, not through the video plane. That path stays inside the app process; it has not
  been run under `--split`.
- **A clip's sound track** plays as of 2026-11-15: see "A clip with sound" below. That clip
  goes through a GL texture, not this video plane; the two have not been run together.
- **Pixels are checked by eye.** The integration test counts frames at the broker. That the
  picture is right was read from screenshots.
- **Each frame is imported by the compositor**, a new `wl_buffer` per frame, with the limit
  noted under C5: a buffer that passes every check and that the compositor still cannot import
  ends the broker's connection.
- **Which `SurfaceView` is video** is decided at launch from its class: the platform
  `SurfaceView` itself and `VideoView` are video, any other subclass is GL. An app that renders
  GL from native code into a plain `SurfaceView` is taken for video; `ACL_SURFACEVIEW=gl`
  overrides. This is not the split's; it is the same single-process.

Two limits found while building this were fixed after it, in both modes: a `SurfaceView` over
an opaque background showed no video, and a `SurfaceView` present at launch was taken for a GL
surface. The fixture app now has an opaque background, and the test runs it with the
`SurfaceView` there at launch and added later.

**S4 as built: audio (x86-64, 2026-11-15).** The child has no audio device and no route to
PipeWire. Every sound a4l makes goes through one interface, `audio::Sink`, and in a split
child `audio::make_sink()` returns a sink that sends to the broker. The broker plays each
stream through the sink it would have used single-process: PipeWire with `ACL_AUDIO=pipewire`,
else the one that counts frames.

- **Three messages.** `AudioOpen` states a stream's id, rate and channel count. `AudioWrite`
  carries interleaved 16-bit samples for it, in the datagram, at most 16 KiB each.
  `AudioClose` ends it.
- **No shared memory.** The earlier sketch had a ring both processes map. A mapping the app
  process can truncate is one the broker can fault on, and audio is small enough to copy:
  about 176 kB a second for 44.1 kHz stereo.
- **One way in.** `libsoundpool.so` carries its own copy of the audio code and cannot see the
  a4l process's channel, so the sink reaches it through four functions the executable exports
  by name (`acl_split_audio_*`). Only those are exported.

What the broker checks (C5):

- **The stream.** An id below 8, a rate from 4000 to 192000 Hz, 1 to 8 channels. At most 8
  open at once, and at most 64 opened over a connection's life, since opening one can hold the
  broker for half a second waiting on the audio daemon.
- **A write.** For a stream that is open, not empty, and a whole number of frames.
- **How much is held.** A write that would put more than 2 s of audio in the sink unplayed is
  dropped and counted. A child that is ahead of the device is not malformed, but how much the
  broker holds is not the child's to decide.

Measured on host ART under headless Weston, confined, filter on. The fixture app
(`tests/fixtures/sound_pool`) loads a 1 s 440 Hz tone and plays it twice through `SoundPool`.
The app process sent 112128 frames at 44.1 kHz; the broker's sink got 112128, with none
dropped and none refused. Single-process the same app writes 112128.

Played through PipeWire into a null sink made for the purpose, and recorded from it, the two
modes give the same sound: a tone of 2.008 s with its spectral peak at 439.2 Hz, the same
level, and the same two gaps of about 2 ms. Nothing was played on a real output.

Tested: unit tests for each refusal; broker tests that send 20000 frames and compare them
sample for sample with what the sink was given, write from a second thread, and exercise the
drop; the fuzz harness with a sink that reads every sample it is handed, 32.9 million
executions in five minutes on eight workers with no finding.
`tests/integration/run_split_audio_test.py` runs the fixture end to end with the counting
sink, so it makes no sound; it needs host ART and so does not run in CI.

Not covered:

- **No word back from the device.** A sink takes what it is given at once, and the child is
  not told how far the device has got. `SoundPool` and `AudioTrack` (below) each pace
  themselves by the clock, which holds as long as that clock and the device's agree. They
  drift apart by parts per million; over hours the broker's 2 s limit would drop a little.
- **Master volume and audio focus** stay unavailable in the child.
- **Recording.** There is no path for audio input in either mode.
- **One app, one format.** `SoundPool` mixes to 44.1 kHz stereo whatever the source; no other
  rate or channel count has crossed outside the unit tests.
- **Latency** was not measured.

**`AudioTrack` (2026-11-15).** The framework's `android.media.AudioTrack` had no native side
on host ART; the first call an app made on it was an `UnsatisfiedLinkError`. It has one now
(`runtime/audio_track_jni.cpp` over `audio::Track`), single-process and under `--split` alike,
since it plays through `audio::Sink`.

- **Blocking writes block.** A track is a bounded buffer that a pump drains into the sink at
  the stream's rate, 30 ms ahead of the clock. A writer waits for room. This is what paces an
  app that writes in a loop, and it needs nothing from the device, so it is the same in a
  split child.
- **The position follows the clock.** A frame counts as played when its time comes, not when
  it is handed to the sink, less whatever the sink says it still holds.
- **A closed stream plays out.** An app closes a track when it has written the last of it.
  The broker keeps a closed stream's sink until it has emptied, at most 2.5 s, instead of
  cutting its tail. Before this, 40 to 70 ms were lost from the end of a track under
  `--split`.
- **Linear PCM only**: 8-bit, 16-bit, 24-bit packed, 32-bit and float, 1 to 8 channels,
  converted to 16-bit. Streaming and static tracks.

Measured on host ART under headless Weston with the fixture app
(`tests/fixtures/audio_track`): 2 s at 48 kHz stereo 16-bit written as `short[]`, `byte[]`
and a direct `ByteBuffer`, then 0.5 s at 44.1 kHz mono float.

| | single process | `--split`, confined |
|---|---|---|
| natives bound | 54 of 54 | 54 of 54 |
| 2 s of blocking writes took | 1895 ms | 1890 ms |
| 0.5 s blocking write took | 394 ms | 394 ms |
| frames at the sink | 96000 + 22050 | 118050 at the broker, 0 dropped, 0 refused |
| playback position at the end | 96000, 22050 | 96000, 22050 |

A blocking write is expected to take the audio's length less the 80 ms buffer and the 30 ms
lead: 1890 ms and 390 ms. Recorded from a PipeWire null sink, three runs in each mode: a
440 Hz tone at peak 8000 for 2.000 s, a silence where the app changes track (27 ms
single-process, 5 to 6 ms split), then 880 Hz at peak 4097 (the float tone at a quarter
scale and half volume) to the end, with no other gap. Nothing was played on a real output.

Not covered:

- **Speed and pitch** (`PlaybackParams`, `setPlaybackRate`) are refused or read back as 1:
  there is no resampler or time-stretcher.
- **Not built:** volume shapers, effects, routing, static-track looping and seeking answer
  with an error or a default. A marker or a position period is stored and its callback
  never fires. An aux-effect send level and an output device are accepted and do nothing.
- **Device latency** is not in the position: what PipeWire holds past its own queue is not
  known to the track. Not measured.
- **Encoded and offloaded tracks** are refused at setup.
- **Eight tracks at once under `--split`**, the broker's limit on open streams, with
  `SoundPool` counting as one. A ninth fails to initialize. Single-process has no limit, and
  each track is a thread.

**Audio decoders in `MediaCodec` (2026-11-15).** `MediaCodec` decoded video only, onto a
Surface. It now decodes audio to byte buffers of 16-bit PCM, in software through FFmpeg, in
the process that asks: under `--split` that is the confined app process, which is where data
an app supplies is parsed. Nothing new crosses the boundary; the PCM goes on to an
`AudioTrack` like any other.

- **Codecs:** AAC, MP3, Opus, Vorbis and FLAC, each listed in `MediaCodecList` only if the
  FFmpeg on the host has its decoder. AAC is listed with the LC, HE and HE v2 object types;
  LC is the one that was run. Codec-specific data is taken as a
  `MediaFormat` carries it; for Vorbis, whose two `csd` buffers FFmpeg wants as three laced
  header packets, the missing comment header is supplied.
- **The buffer model** is `media::AudioCodec`, with no JNI in it: four input buffers and
  eight output buffers. An input buffer stays in flight until its access unit is decoded and
  decoding stops when every output buffer is held, so a client that queues without reading
  waits; nothing grows.
- **Both ways of driving it:** `dequeueInputBuffer` and `dequeueOutputBuffer`, and a
  `MediaCodec.Callback`. The format is announced before the first buffer and on a change,
  the stream ends with one flagged buffer, and `flush()` holds callbacks until `start()`.

Measured on host ART. The decoder, on a 1 s 440 Hz stereo tone the `ffmpeg` CLI encoded
(`tests/integration/run_audio_decode_test.py`):

| Codec | access units | frames out | rate | measured |
|---|---|---|---|---|
| AAC (ADTS) | 48 | 49152 | 48000 | 440.4 Hz |
| MP3 | 41 | 47232 | 44100 | 439.8 Hz |
| Opus (Ogg) | 51 | 48648 | 48000 | 440.1 Hz |
| Vorbis (Ogg) | 49 | 48704 | 48000 | 439.6 Hz |
| FLAC (Ogg) | 11 | 48000 | 48000 | 439.0 Hz |

End to end, a fixture app (`tests/fixtures/audio_codec`) finds the AAC decoder through
`MediaCodecList`, decodes a 2 s tone of 95 access units into an `AudioTrack`, flushes, and
decodes the first ten again. Driven by dequeue calls and by a callback, single-process and
under `--split` confined, all four runs give the same: 97280 frames and the end of the stream,
then 10240 frames after the flush, the track's position at 107520 of 107520, and under
`--split` 107520 frames at the broker with none dropped or refused. Recorded from a PipeWire
null sink, single-process and split give the same 2.219 s of 440 Hz.

Not covered:

- **`MediaExtractor` and `MediaPlayer`** have no native side. An app that leaves the
  container to the platform cannot open a file.
- **The legacy buffer arrays** (`getInputBuffers`, `getOutputBuffers`) are not provided;
  `getInputBuffer(index)` and `getOutputBuffer(index)` are.
- **Output is 16-bit.** A stream decoded to float or 24-bit is converted down. A frame of
  more than eight channels is dropped.
- **Other codecs:** AC-3, E-AC-3, DTS, AMR, ALAC, raw PCM and anything encoded for
  passthrough are not listed. No encoder.
- **Only AAC went through the Java API.** The other four are tested at the decoder, below
  JNI.
- **Decoder flaws.** FFmpeg parses the stream in the app process. Under `--split` that
  process is the sandbox; single-process it is a4l itself.

**A clip with sound (2026-11-15).** A Flutter app whose content is the `video_player` plugin
(media3's ExoPlayer) playing a bundled 5 s clip, H.264 320x240 with a 440 Hz AAC stereo track
(`tests/fixtures/flutter_video_player`). ExoPlayer parses the MP4 itself and uses
`MediaCodecList`, `MediaCodec` in callback mode for both tracks, and `AudioTrack`. The same
app played video alone in July; with a sound track in the clip it met two more gaps:

- **Audio HAL queries.** Before it picks an audio renderer, media3 asks
  `AudioSystem.getParameters` and `native_get_offload_support`, which had no native side; the
  playback thread died of an `UnsatisfiedLinkError`. They, and the neighbors a player asks
  (direct playback support, the primary output's rate and buffer, latency), now answer that
  nothing is offloaded or passed through and describe the output a track plays into.
- **A `@CriticalNative` signature.** `HardwareBuffer.nEstimateSize` is `@CriticalNative`; a4l's
  native took a `JNIEnv*` and a class as well, so it read the handle from the wrong register.
  It crashed on the raster thread in the first seconds of playback (`SIGSEGV` at `0x12`).

Measured on host ART, single-process and under `--split` confined, the same in both:

| | |
|---|---|
| decoders ExoPlayer chose | `acl.h264.decoder`, `acl.aac.decoder`, both in callback mode |
| position the player reports | 0 to 5000 ms, about 500 ms per half second, then completed |
| frames at the audio sink | 240640 (235 AAC frames of 1024; 5.013 s at 48 kHz) |
| recorded from a PipeWire null sink | 4.997 s of 440 Hz, no gap, the same in both modes |

ExoPlayer takes its clock from the `AudioTrack`, so the position advancing in real time is
also a check of the track's position. A screenshot from a headless sway during playback under
`--split` shows the test pattern at clip time 3.267 s.

Not covered:

- **Synchronization** was measured afterwards and was wrong; see "Picture against sound"
  below.
- **One clip, one player.** Local file, no seeking, pausing or looping, no streaming, no
  network. The app is not in the test suite: building it needs a Flutter SDK.
- **The video plane.** `video_player` shows its frames through a GL texture. A player that
  uses a `SurfaceView` (a media3 `PlayerView`) was run afterwards; see "A `SurfaceView`
  player" below.

**Picture against sound (2026-11-15).** Measured on the same app with a clip that flashes
white and beeps at the same instants: a point of the screen and the sink's monitor captured
on one clock (`tools/measure-av-sync.py`), and a reference player, mpv, through the same
capture to take out the capture's own delay. Audio minus video, positive when the sound is
late:

| | ms |
|---|---|
| mpv, four runs | +16, +18, +16, +18 |
| a4l as it was, single process | +277 to +342; five flashes seen for two shown |
| with decoder surfaces held | +75, +73 |
| and frames shown at the time asked: three runs | +18, +26, +21 |
| the same, a clip with eight events | +26 |
| `--split`, five runs of the eight-event clip | +30, +24, +35, +44, +21 |

The sound was about 280 ms behind the picture, and frames were shown out of order. Three
causes, none of them in the audio path, whose clock was checked against the wall clock and
found exact:

- **A queued frame showed a later picture.** A decoded frame is passed on as a descriptor of
  the decoder's surface. Nothing kept the surface: the next `receive_frame()` returned it to
  FFmpeg's pool, the decoder drew a later picture into it, and a frame still waiting in the
  codec's output queue, up to eight deep, showed that picture. A frame now carries a hold on
  its surface (`gfx::DmabufFrame::hold`), kept by whoever keeps the frame: a queued output
  buffer, an imported `AHardwareBuffer`, a `wl_buffer` the compositor has not released, and
  under `--split` the last eight frames sent to the broker (since 2026-11-19: each frame
  until the broker says it is released, see "A video frame is released"). The decoder's pool
  is larger by the same count (24 extra surfaces, was 12).
- **Frames were shown as soon as released.** A player releases a frame with the time it is
  to be shown, and ahead of it: media3 by 45 ms at the median here, 50 at most. Android holds
  the buffer until then. a4l showed it at once. `MediaCodec.releaseOutputBuffer(index,
  timeNs)` now waits for the time (`media::TimedRelease`).
- **Under `--split` the video decoder sometimes would not start.** When the output surface
  is not there yet, media3 makes a placeholder surface with Java EGL14. A split child has no
  display server, so `eglInitialize` failed; with that fixed, no config matched, because the
  app asks for a window config and Mesa's surfaceless platform has pbuffer configs only.
  EGL14's default display is now the surfaceless platform where there is no display server,
  and a window config request falls back to a pbuffer one. Before, three runs of four took
  this path and failed; after, five of five complete, four of them through it.

Not covered:

- **The capture's resolution.** The screen is sampled every 16 ms, and a frame is on screen
  for 33. Differences under about 15 ms are not resolved.
- **Real outputs.** A null sink and a headless compositor have no latency of their own. A
  real display adds a frame or two, which is not measured. A real audio device adds its
  buffer; see "Device latency" below.
- **The video plane.** These frames go through a GL texture. A `SurfaceView` player was
  measured afterwards; see "A `SurfaceView` player" below.
- **Seeking, pausing, speed.**

**Device latency (2026-11-15).** `AudioTrack`'s position counted what PipeWire held in a4l's
queue and nothing further on, so a device with a delay of its own put the sound late by that
delay. Measured with a sink that delays 200 ms and says so (a `pw-loopback --delay 0.2` into
the null sink, with `ProcessLatency` set to match), same clip, same capture. Audio minus
video, ms:

| | ms |
|---|---|
| mpv, null sink | +4 |
| mpv, delayed sink that does not declare its delay | +202 |
| mpv, delayed sink that declares it | -8 |
| a4l before, delayed sink: two runs single process, two `--split` | +221, +224; +215, +232 |
| a4l, delayed sink, single process, five runs | +13, +12, +12, +20, +20 |
| a4l, delayed sink, `--split`, five runs | +16, +34, +53, +18, +36 |
| a4l, null sink: two runs single process, two `--split` | +29, +25; +4, +18 |

Each a4l figure is the mean of a run's two events. So the delay is taken out: a4l is within
about 30 ms of the reference single-process and 60 under `--split`, where it was 220 late.

- **The sink says how late its device is.** `audio::Sink::latency_ns()`. The PipeWire sink
  reads `pw_stream_get_time_n` in its process callback: the graph's delay to the device plus
  what the stream's resampler holds.
- **`AudioTrack.getTimestamp` counts it.** The frame given is the position less the latency.
  The position itself does not count it, as on Android, and `getLatency` (not public API, but
  players call it) reports the buffer plus the device.
- **Under `--split` the broker tells the child.** The child cannot see the device. The broker
  sends `AudioLatency` (stream, microseconds, at most 2 s) once the device has a figure, and
  again when it moves by 2 ms: at most one message per write the child made, none for a
  steady device. Protocol version 6.
- **Two rules the first attempt was missing.** In two `--split` runs of three the delay was
  not taken out at all. The player had taken its first timestamp before the latency was
  known; when the figure arrived the timestamp's frame count stepped back by 200 ms, which a
  player reads as a counter that wrapped, and it stopped using timestamps. So: no timestamp
  is given until the device has said its latency, for at most 250 ms (a child whose main
  thread is busy never hears the broker, and goes on without the figure); and a timestamp's
  frame never goes back, so when the latency grows the frame stands still until playback has
  passed it.

Not covered: a real device (the delayed sink is a loopback, not Bluetooth or HDMI); a device
that changes during playback, which is covered by a unit test and was not measured; the
display's own latency; `SoundPool`, which has no timestamps to correct.

**A `SurfaceView` player (2026-11-15).** A media3 `PlayerView` on its default `SurfaceView`
(`tests/fixtures/media3_player_view`), same clip and capture. It had no picture: 0 flashes
seen for 2 beeps heard, in both modes. A `PlayerView` lays its `SurfaceView` out again once it
knows the picture's aspect ratio. a4l had set the view's `mHaveFrame`, so that layout ran
`SurfaceView.updateSurface`, which found no window surface, told the player its surface was
destroyed and threw on `Surface.nativeForceScopedDisconnect`, once per frame. a4l now leaves
`mHaveFrame` unset and reports the new size to the app's `surfaceChanged` itself. Audio minus
video after, ms:

| | ms |
|---|---|
| mpv, two runs | +26, +24 |
| a4l, single process, five runs | +42, +51, +43, +51, +49 |
| a4l, `--split`, four runs | +51, +37, +49, +29 |

Each a4l figure is the mean of a run's two events. The sound is about 20 ms later against the
picture than the reference's, in both modes; a frame of the clip is 33 ms. The `video_plane`
fixture now lays its `SurfaceView` out again while it plays, and `split-video` fails on the
old code. Not covered: seeking, pausing, speed; a `PlayerView` in a Compose screen; a
`GLSurfaceView` laid out again, whose path sets the same field and was not changed.

**Pause, seek, speed (2026-11-15).** Both player apps were scripted to pause for 1.5 s, seek
back, and seek back again at twice the speed, with the clip's two events after each. On the
`PlayerView` app, before: after the pause the sound came 1.5 s after the picture; after the
seek the player never left buffering. After, audio minus video in ms, two events per run
(mpv playing straight through: +14 +20, +14 +22):

| | after a pause | after a seek | at twice the speed |
|---|---|---|---|
| `PlayerView`, single process, four runs | +30 +43, +40 +35, +44 +46, +53 +44 | +71 +68, +73 +70, +60 +57, +58 +55 | +48 +62, +64 +62, +44 +49, +62 +68 |
| `PlayerView`, `--split`, three runs | +51 +47, +41 +36, +41 +36 | +80 +73, +80 +76, +84 +79 | +61 +59, +70 +68, +67 +64 |
| `video_player`, single process, two runs | +30 +27, +43 +41 | +61 +60, +48 +47 | +38 +37, +32 +30 |
| `video_player`, `--split`, two runs | +31 +27, +41 +38 | +61 +58, +53 +52 | +48 +46, +47 +45 |

At twice the speed the flashes are 96 ms long and 1.25 s apart, as they should be.

- **A `SurfaceView`'s frames were shown on a4l's clock, not the player's.** The video plane
  fed the decoder each packet when its timestamp fell due on the wall clock, showed each
  frame as it was decoded, and handed the player an empty buffer to release. A paused player
  stops releasing; a4l went on showing. Frames for the plane now wait in the codec's output
  slots, holding their decoder surfaces, and go to the plane when the player releases them,
  at the time it gives (`media::TimedRelease`), as frames for a texture already did. The
  fixed pool of eight slots is what holds the decoder to the player's rate.
- **`MediaCodec.flush()` did not flush a video codec.** It dropped the output slots and
  nothing else. The input buffers the player had been offered stayed marked as held, so none
  was offered again and the player waited. Flush now empties the packet queue, frees every
  input buffer and output slot, cancels frames waiting for their time, flushes the FFmpeg
  decoder (`CodecDecoder::flush`), and in asynchronous mode delivers nothing until `start()`.
  It takes a lock the decode thread holds for each pass, so it does not run into one.
- **Input is bounded.** With no pacing a player fills every buffer it is offered; buffers are
  offered only while fewer than 16 packets wait.
- **`MediaCodec.setParameters`** was missing; a player calls it when the speed changes. It is
  taken and ignored.
- **A rebuilt debug Flutter app ran its old code.** Found on the way: Flutter's debug-mode
  extractor keeps its copy of the Dart code while the package's version code and
  `lastUpdateTime` are unchanged, and a4l reported 0. `PackageInfo.lastUpdateTime` and
  `firstInstallTime` are now the APK's modification time.

After a seek the sound is about 30 ms later against the picture than before it, in both
apps and both modes. That is AAC, not a4l's timing. An AAC decoder's output for a packet
is the sound of one frame earlier (1024 samples, 21.3 ms at 48 kHz); the file says so as an
encoder delay, and media3 trims that frame at the start of the stream and not after a seek,
where it takes the first buffer's timestamp as it comes. The same clip with FLAC audio, which
has no such delay, shows no shift. `PlayerView`, single process, mean of the two events
after the pause and after the seek, ms:

| audio | after the pause | after the seek | difference |
|---|---|---|---|
| AAC, three runs | +40 | +65 | +26 |
| FLAC, four runs | +50 | +40 | -11 |
| FLAC, `--split`, three runs | +60 | +67 | +7 |

Run to run a figure moves by about 15 ms, so -11 and +7 are no shift and +26 is one. a4l's
decoders label a buffer with its packet's timestamp, as `MediaCodec` does, and that is left
as it is: shifting the label would break the trim at the start. Whether a device's own AAC
decoder gives the same 21 ms after a seek was not measured; nothing in how media3 handles
the buffers suggests otherwise.

Ruled out on the way: the audio clock. The PipeWire sink's pulls are regular from a
stream's first one (21.33 ms apart, no burst at the start), and anchoring `AudioTrack`'s
timestamps on those pulls instead of on the track's own count changed nothing measurable, so
that change was not kept.

Not covered: seeking forward into data not yet read, seeking while paused, speeds other than
2, a real display.

**Window size (2026-11-15).** An a4l window opens at a4l's size and, until now, kept it:
`xdg_toplevel.configure`'s width and height were ignored on every path. A GL app in a single
process now follows a resize. The presenter reports a size the compositor asks for after the
first (`IPresenter::take_resize`); the GL session resizes its `wl_egl_window`, reports
`surfaceChanged` to the `SurfaceView`'s holder, and lays the decor out at the new size.
Checked under headless sway with the window floated and set to 800x600, 1000x700 and
640x480: a `GLSurfaceView`'s renderer gets `onSurfaceChanged` at each and its picture fills
the window; a Flutter app does the same and re-centers its content.

**Window size under `--split` (2026-11-15).** A GL app in the confined child follows a
resize too. The broker holds the window, so the size is the broker's to pass on: it sends the
child `SurfaceResize`, the child's holder reports `surfaceChanged`, and the app builds a new
window surface, which a4l makes at the new size. A gbm surface cannot change size in place,
so the old one is left alone until the app replaces it; `GLSurfaceView` and Flutter both do.

The broker passes on one size at a time. The next is not sent until a frame of the last has
been accepted, so a frame is only ever one of two sizes: the surface's, or the one the child
has been told to take. Both are the compositor's. The first frame of the new size makes it
the surface's, and the size left behind is refused from then on. A child that does not
follow keeps its size, and the compositor shows a window of another size than it asked for.

Checked under headless sway, window floated and set to 800x600, 1000x700 and 640x480:

| App | Result |
|---|---|
| `GLSurfaceView` fixture, `--split` | `onSurfaceChanged` at 800x600, a frame of that size at the broker, 0 messages rejected (`gl-surface-resize`, then part of `gl-surface-relayout`) |
| Flutter `video_player` app, `--split` | a frame of each of the three sizes at the broker; each screenshot is filled to the window's size |

Found on the way: in most `--split` runs of the Flutter app the player reported its video
as 0x0 when it initialized, and the app then laid the video out square (700x700 in the
1000x700 window, where a single process shows 933x700). Of the runs logged on 2026-11-15,
15 of 23 under `--split` reported 0x0 and 0 of 26 in a single process did. It was not the
resize and not the boundary; see "A player that starts without its surface" below.

**A player that starts without its surface (2026-11-16).** `video_player` calls
`prepare()` and then `setVideoSurface()`, in that order, on the app's main thread. If
ExoPlayer's playback thread gets to its video codec before the surface arrives, it starts
on a placeholder surface, counts as ready without showing a frame, and reports a video size
of 0x0, which the plugin passes on once and never corrects. In the logs the two go together
without exception: all 16 runs that reported 0x0 made the placeholder, and none of the 37
others did. One single-process run did it too, once the timing was disturbed.

The plugin gets away with this on a device, presumably because an `ImageReader` and its
surface cost the main thread little there (not measured). In a4l they cost it 28 ms (median of 5 runs, from the end of the player's
constructor to the surface being handed over), against 36 ms for the playback thread to
reach its codec. Two things took the time, both on the main thread:

- Loading `media_jni` registered a4l's natives and, in the same call, asked FFmpeg which
  audio decoders it has, which loads FFmpeg: 8 ms. The list is now built when it is first
  asked for, which is on the playback thread.
- A new `ImageReader` allocated all its buffers at once, seven dma-bufs for a player's 1x1
  reader that are never drawn into: 12 ms. The queue now allocates the first and makes the
  rest when a producer asks.

| | Surface handed over, ms | Codec's surface decided, ms | 0x0 |
|---|---|---|---|
| Before, machine under other load, 5 runs | 28.2 (25.5 to 70.8) | 36.1 | 3 |
| After, `--split`, 20 runs | 8.5 (7.8 to 9.4) | 34.0 | 0 |
| After, single process, 10 runs | 8.8 (8.4 to 9.6) | 33.9 | 0 |
| After, `--split`, 40 busy loops running, 20 runs | 30.9 (14.7 to 44.2) | 117.7 (least 101.3) | 0 |

Times are from the end of the player's constructor; median, with the range in brackets. The
race is the plugin's and is still there. What changed is the margin: about 25 ms now, where
there were 8 and that was not enough. The decision time is when the choice shows in the log, which is later than
when it is made, so the true margin is smaller than the table reads. The same unfixed build
under 40 busy loops reported 0x0 in only 1 of 20 runs, so a count of failures says little
about this race in either direction; the main-thread time is the measurement to go by.

**Window size for View-tree apps (2026-11-16).** An app drawn through the View tree (Views,
Compose) follows a resize as well, in a single process and under `--split`. On Android a
window that changes size is a configuration change, and that is how the app is told
(`HostBootstrap.resizeWindow`): every `Resources` the app can reach takes the new screen
size, the application's callbacks and each activity get `onConfigurationChanged`, the view
tree is told and laid out again. Underneath, the session takes the size, and the render
thread's next frame makes a render target and dma-bufs of it (`resize_targets` in
`tools/aosp/graphics_jni/gpu_dmabuf.cpp`). Under `--split` nothing new crosses the boundary:
the child is told by the same `SurfaceResize`, and its frames are checked by the same rule.

Checked under headless sway, window floated and set to 800x600, 1000x700 and 640x480:

| App | Single process | `--split` |
|---|---|---|
| `view_resize` fixture (`view-window-resize`), 800x600 only | application, activity and view told 800x600; picture is 800x600 | the same; a frame of that size at the broker, 0 rejected |
| forkyz (Compose) | laid out again at each size, picture fills the window | screenshots identical to single process |
| foss4g schedule (Views, AppCompat) | laid out again at each size | identical but for 100 pixels |
| octalsuntime (custom View) | picture fills the window at each size | not run |
| media3 `PlayerView` app | video rectangle 800x600, 933x700, 640x480 | the same |

What this is not:

- **No relaunch.** Android destroys and creates an activity again on a size change unless
  its manifest says it handles one. a4l cannot relaunch an activity, so every activity is
  treated as if it had said so. An app that relies on the relaunch to pick up a
  size-qualified resource (a `layout-sw600dp`) keeps the layout it inflated.
- **The window is the display.** a4l advertises one size for both, so `Display` and
  `DisplayMetrics` follow the window. On Android a freeform window's display stays put.
- **Deferred present only.** With `ACL_PRESENT_IMMEDIATE` the app's `Canvas` wraps the
  render target's first `SkCanvas`, and the size is not taken up.

Three faults found on the way, each fixed:

- `getResources().getDisplayMetrics()` said 0x0 from the start, resize or not. The framework
  copies `noncompatWidthPixels` over `widthPixels` for an app that supports its screen, and
  a4l left the former at 0.
- a4l never set its window geometry, so it was the bounding box of the window and its
  subsurfaces. A video laid out past the window, or still at its place from before the
  resize, made the window larger than its buffer, and sway configured it to that: one resize
  to 800x600 came back as 800x600, 1173x800, 800x600. The geometry is now the toplevel's
  rectangle.
- The video plane was placed only when a frame arrived. With the video paused or ended, a
  resize or a layout change left the picture where its last frame had put it. The frame on
  the plane is now placed again when the rectangle or the window changes.

**The first size (2026-11-16).** a4l opens at the output's size, and a tiling compositor's
first configure names the tile. That first size was recorded as already taken, so only a later
change was a resize, and an app in a tile smaller than the output was drawn at the output's
size and clipped. The size a4l opens at is now the one recorded, so a first configure that
differs goes down the same path as any other resize, single-process and under `--split`. The
app still starts at the output's size and is told the tile's on its first dispatch; it is not
created at the tile's size. Checked under headless sway (1280x800 output): a 1200x720 tile
(40-pixel gap) for forkyz, the GL fixture and the Flutter `video_player` app in both modes, and
two 640x800 tiles side by side (forkyz single-process, foss4g under `--split`), with no message
rejected. The `view-window-resize` test runs the gapped tile in both modes. A lone tile with no
gap is the output's size and was never affected.

**On an embedded shell (2026-11-16).** Run against `ihs_wl_server` (ivi-homescreen's embedded
Wayland server, release 0.2.0) under a stand-in shell that hosts one view and resizes it; the
stand-in is not in this repository. The server advertised one modifier for ARGB8888, LINEAR.

- A View-tree app under `--split` showed nothing: 0 frames in 24 s. The render targets took the
  GPU's first tiled modifier without asking what the compositor advertises, and the broker
  refused every frame ("modifier not advertised"). Single-process sent the same unadvertised
  modifier and this server accepted it; a strict compositor need not. The targets now take a
  modifier from the compositor's list: tiled if one is listed, else LINEAR. With none of the
  GPU's listed they keep the old choice and say so. sway still gets the tiled modifier (6
  listed).
- After the change: forkyz and the Flutter `video_player` app, both modes, bound to a 1024x600
  view, took 800x480 and 1280x720 within 30 to 50 ms of the view's resize, and settled on the
  last size after 180 resizes in 4.4 s (33 ms apart, then 16 ms apart). Single-process took all
  180; `--split` took 131 (forkyz) and 105 (video), since the broker passes on one size at a
  time. No message rejected.
- The first frame or two were at the output's size (1920x1080 here), scaled into the view by
  the shell: single-process for every app, and under `--split` for the Flutter app, which
  draws before the broker's resize arrives. The first size is now taken when the session
  opens, before the app draws. Under `--split` the broker sends it ahead of its answer to
  `SurfaceCreate`, and the child has it when its presenter opens. First frame at the view's
  1024x600 in 3 of 3 runs each: forkyz, the GL fixture and the Flutter app, both modes.
- The media3 `PlayerView` app ended at its first video frame, single-process. The frame is
  NV12, the server advertised no NV12, and sending it is a protocol error that closes the
  connection. A video frame whose format or modifier the compositor does not advertise is now
  dropped, with one warning (under `--split` the broker already refused the frame).
- **Video for a compositor without the decoder's format (2026-11-16).** Once a format has been
  turned away, each frame of it is drawn on the GPU into an XBGR8888 LINEAR dma-buf
  (`gfx::VideoConverter`: the frame as an external texture, one pass into one of 12 pooled
  buffers) and that goes to the video plane. The presenter says a format was refused; under
  `--split` the broker tells the child with a new message, `VideoRefused` (protocol version 7),
  once per format. The frames sent before the refusal is known are lost: one single-process,
  those in flight under `--split`.
  - On `ihs_wl_server`: the media3 `PlayerView` app's video is a second layer under the
    window at 800x600, 640x480 and 960x720 as the view is resized, both modes. That server
    does not draw, so the picture itself was not seen there.
  - The picture: a four-color clip (one color a quadrant) converted through the decode probe
    comes out with each quadrant within 1 of its source color and the right way up.
  - Cost: 0.18 ms a frame at 320x240 and 0.56 ms at 1920x1080 on a Ryzen 9 9950X's integrated
    Radeon (radeonsi), `glFinish` included.
  - `split-video` has a third run with every frame converted (`ACL_VIDEO_CONVERT`): 154 of
    155 frames at the broker's video plane under Weston, 0 rejected.
  - Picture against sound (2026-11-16, headless sway, null sink, the media3 `PlayerView` app,
    audio minus video in ms, each figure the mean of a run's two events, three runs each
    taken in turn; mpv on the same capture +22 and +25): single process +34, +50, +28 as
    decoded and +42, +33, +40 converted; `--split` +54, +61, +55 as decoded and +41, +55, +43
    converted. Conversion moves the picture by less than the 20 ms the runs differ among
    themselves.
  - The fuzz harness's stand-in compositor now takes XBGR8888 LINEAR on the video plane, and
    two seeds send converted frames, good and malformed. With the validator's stride check
    removed the harness fails on a seed. 6.3 million runs in seven minutes from the 21 seeds,
    no crash.
  - Not done: 10-bit is converted to 8; the YUV matrix and range are the GL driver's defaults,
    with no hint from the stream; a compositor that
    lists neither the decoder's format nor XBGR8888 LINEAR still gets no picture; the texture
    path (Flutter) does not go through the video plane and never needed this.
- Input: a pointer tap at (300,200) in the view reached a View below the 64-pixel title bar as
  ACTION_DOWN and ACTION_UP at (300,136), both modes.
- `ACL_PRESENT_LINEAR_ONLY` makes a4l act as if the compositor listed LINEAR alone. The
  `view-window-resize` test runs with it in both modes under sway and fails on the graphics
  library from before the modifier change.
- Not run on it: a real shell on a display, touch (as opposed to pointer) input, keys.
- **A video frame in two dma-bufs (2026-11-18).** A decoder may keep luma in one buffer and
  chroma in another. `VideoFrame` now says which of the datagram's two descriptors each plane
  is in (`buffer[]`, protocol version 8); before, the child did not send such a frame. The
  broker examines both descriptors, holds each plane to the size of the buffer it names, and
  refuses a frame that names a descriptor that did not come or leaves one that came unnamed.
  A frame in three buffers is still not sent.
  - Checked by unit test and by the fuzz harness (a seed with two descriptors; 150 s of
    mutation, about 2 million runs, no finding).
  - Seen on a screen the same day, on a Pi 5 (HEVC, 640x360 test pattern, HDMI): a frame
    with a buffer per plane crosses and reads the same as single-process, under Weston 14
    and under cage (wlroots 0.18), 300 frames taken and none refused.
- **Broadcom's column format (2026-11-18).** A Pi 5's HEVC decoder gives NV12 in columns 128
  bytes wide (`DRM_FORMAT_MOD_BROADCOM_SAND128`), and a plane's stride is the height of its
  columns in rows. The broker checked every NV12 frame as rows of `stride` bytes and refused
  these. For that modifier it now checks that a column holds the plane's rows and that the
  last column ends inside the buffer the plane names.
  - The fuzz harness's stand-in compositor lists the modifier and walks every column of a
    frame that reaches it. With the column term taken out of the check the new seed fails.
    150 s, 2.4 million runs, no finding.
  - On the Pi 5 the video fixture (rebuilt with an HEVC clip) plays through `--split=unconfined`
    with the picture right: 181 units fed, 360 presents, p50 16.7 ms. `--split` itself has no
    system call list for arm64.
- **A video frame is released (2026-11-19).** The child kept the decoder surfaces of the last
  eight video frames it sent, whatever the broker had done with them, because nothing told it
  when one was free. A decoder with a small fixed pool then ran dry: GStreamer's OpenMAX
  element on an SA8155P board has 9 or 10 output buffers and takes no request for more, and
  with its frames passed on uncopied, playback stopped at the first flush. `VideoFrame` now
  carries an id the child chooses, and the broker answers `VideoReleased` with it when the
  presenter drops the frame's hold, which is when the compositor has released the buffer or
  the frame was dropped unshown (protocol version 9). The child keeps a surface until that
  answer, in a table of 16; a broker that stops answering costs the oldest entry, as before.
  The id is the child's and the broker only repeats it, so it is not validated; id 0 means the
  child keeps nothing and gets no answer.
  - Checked by unit test (a frame the compositor keeps holds its surface; released, it does
    not) and by the fuzz harness, whose video seeds now carry an id (91 s of mutation, about
    1.4 million runs, no finding).
  - On the board, GStreamer forced: a 1920x1080 stream with B frames passes 114 of 120
    frames on without a copy, single process and under `--split` alike; before, `--split`
    stopped at the flush with more than 5 frames out.

**C5 as built (x86-64, 2026-11-14).** What the broker does with each thing the child can send:

- **A frame.** The broker asks the kernel, not the child, what the descriptor is: `fstatfs` must
  report the dma-buf filesystem and `lseek` gives the size. The frame is then refused unless its
  width and height are the surface's (or the one size the broker has told the child to take;
  see "Window size under `--split`"), its format is ARGB8888 or XRGB8888, its modifier is
  linear or one the compositor advertised, it has at most four planes, plane 0's stride holds a
  row and its last row ends inside the buffer (computed in 64 bits), each auxiliary plane has a
  stride and starts inside the buffer, and the slot is in range. A refused frame is counted by
  reason, logged once per reason, and answered with a keepalive so the child's frame loop does
  not hang.
- **A video frame, a video rectangle.** See "C3 as built: video" above.
- **An audio stream, an audio write.** See "S4 as built: audio" above.
- **A surface.** Size bounded as before. At most 16 over a connection's life, since each is a
  toplevel on the user's screen.
- **The window's identity.** The app id is the package name from the APK the broker admitted,
  not what the child sends. The title is the child's, reduced to valid UTF-8 with control,
  bidirectional-override and zero-width characters removed.
- **Descriptors.** Every descriptor that arrives is closed after its message is handled,
  including those attached to a datagram too short to be a message and those beyond the two a
  message carries.
- **Volume.** The broker handles at most 64 messages before it returns to the compositor's
  events, so a flooding child cannot starve its own window's close button.
- **Everything else** (unknown tags, tags that only the broker sends) is counted and dropped.

Tested three ways. Unit tests reach every refusal, with a real dma-buf from `udmabuf` where the
host allows one. A seeded run sends 24000 damaged datagrams through the broker to a presenter
that counts anything malformed reaching it and checks that no descriptor is kept. A libFuzzer
harness drives the same loop under AddressSanitizer and UBSan: 67.9 million executions in ten
minutes on eight workers, with no crash, no sanitizer report and no descriptor kept. With the
buffer-bounds check removed, the same harness fails on its seed inputs.

Measured on host ART under headless Weston, 20 s each, confined: `octalsuntime` 1177 frames,
`forkyz` 1167, `foss4g` 2, with no frame refused; the counts before validation were 1179, 1169
and 2.

Not covered:

- **The compositor's own import.** The presenter builds the `wl_buffer` with `create_immed`. A
  real dma-buf that passes every check here and that the compositor still cannot import can end
  in a protocol error, which closes the broker's connection: the app loses its own window and
  nothing else. Importing asynchronously (`create` and its `failed` event) would turn that into
  a dropped frame.
- **Auxiliary planes** are checked for position only; their layout is the driver's.
- **The fuzzing in CI is a regression check, not a campaign.** Since 2026-11-16 the `present`
  job builds the harness with clang under ASan and UBSan and runs it for 120 s from the seeds on
  every push and pull request. No corpus is kept between runs, so each starts where the last
  one did. The presenter behind it is a stand-in, so libwayland and the compositor are outside
  it.
- **arm64.** `runtime/split_vm.cpp` receives frames from `acl-art-host` with none of this.
- **The clipboard.** The child may set it, up to 60 KiB, as any Wayland client with input focus
  may. It cannot read it.

**Follow-on.**
- The seccomp allowlist is recorded from the app corpus (docs/app-corpus-status.md), not written
  by hand, then reviewed. A hand-written list is wrong in both directions. As built, 98 of its
  175 entries are recorded and 77 are review additions; widening the recording (input, network,
  the full corpus) is what moves entries from the second group to the first.
- The multi-app decision in Phase 5 is constrained by Decision 1: however apps map to processes,
  no app code holds the display connection. Per-app UIDs and binderfs (ADR-0003) remain open and
  are not needed for one confined app.
- ADR-0003 stands for services: the hub stays in-process with the framework, in the child. What
  moves to the broker is the device edge the split already moved.

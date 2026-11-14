<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0009: Containment -- a process boundary at the dma-buf seam; seccomp alone rejected

- **Status:** Accepted (boundary decided; the process split runs on x86-64 as `--split`, with
  the child confined by namespaces and a seccomp allowlist; C3 and C5 are open, so untrusted
  APKs are still not a supported use). Refines ADR-0003.
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
cross; audio, native GL and video do not. The child is confined as of the same date (C4).

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
| C3 | Native GL and video cross the boundary as dma-buf | not designed; see below |
| C4 | Child confined per Decision 2 | x86-64, 2026-11-14: `--split` starts the child through bubblewrap (`src/confine/`) in its own user, mount, pid, ipc, uts and network namespaces, and the child installs a seccomp allowlist on itself before it opens the APK. See "C4 as built" below for what it can reach and what is not covered. arm64: none |
| C5 | Broker validates every message; receive path fuzzed | partial: the `--split` broker bounds-checks sizes, plane count, slot and strings and drops what fails (`wm/split_present.cpp`). Not checked: that the fd is a dma-buf, the format and modifier against what the compositor advertised. Not fuzzed |

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
  with the socket in `ACL_SPLIT_FD`; audio has not moved yet. On arm64 it is all-bionic and needs no `%fs` bridge.

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
- **seccomp.** An allowlist of 173 system calls installed by the child on itself, after a4l has
  started and before it opens the APK; a call off the list answers `ENOSYS`
  (`ACL_SPLIT_SECCOMP=kill` kills instead). 84 were recorded from the five corpus APKs
  (`tools/record-seccomp.py`); 89 were added in review as fallbacks and siblings of recorded
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
- **The recording is narrow.** Five APKs, no injected input, no reachable network. A call an app
  makes only on a touch or a completed connection may be off the list; it fails with `ENOSYS`.
  Nobody outside the project has reviewed the list.
- **No audio, no system services.** With no D-Bus, connectivity, battery, power, Bluetooth,
  storage and service discovery fall back to their null backends, and the keystore is in memory.
  These move to the broker with S4, not into the sandbox.
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
- **arm64** has no system call list and no split child that renders.

**Follow-on.**
- The seccomp allowlist is recorded from the app corpus (docs/app-corpus-status.md), not written
  by hand, then reviewed. A hand-written list is wrong in both directions. As built, 84 of its
  173 entries are recorded and 89 are review additions; widening the recording (input, network,
  the full corpus) is what moves entries from the second group to the first.
- The multi-app decision in Phase 5 is constrained by Decision 1: however apps map to processes,
  no app code holds the display connection. Per-app UIDs and binderfs (ADR-0003) remain open and
  are not needed for one confined app.
- ADR-0003 stands for services: the hub stays in-process with the framework, in the child. What
  moves to the broker is the device edge the split already moved.

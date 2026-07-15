<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original bring-up plan; grounds on plan.md §2/§4 (architecture, phasing),
     docs/art-host-spike.md (host-ART model), ADR-0001 (DEX on host ART), ADR-0004 (Skia+Vulkan
     dma-buf), ADR-0006 (windowing/scaling), cross/rk3588.ini + cross/sa8155p.ini; AOSP build
     system (Soong, host targets), Mesa V3DV/Panfrost docs (public). No GPL-3.0 sources. -->

# Running acl on a Raspberry Pi (and RK3588) — bring-up plan

Target boards: **Raspberry Pi 4** (BCM2711, Cortex-A72 ×4, VideoCore VI) and **Pi 5**
(BCM2712, Cortex-A76 ×4, VideoCore VII). Both are aarch64/Linux — the same architecture family
acl's automotive board targets already assume (`cross/rk3588.ini`, `cross/sa8155p.ini`). This doc
is the ordered, honest task list to stand acl up on a Pi, plus the **RK3588 gaps** (§7).

## 0. TL;DR

The C++23 core cross-compiles to arm64 today — that is not the work. The work is dominated by one
thing: **there is no stock arm64-Linux "host" ART build.** acl's model (ADR-0001) runs the app's
DEX on a real AOSP ART built for the *build host* — currently `out/host/linux-x86` (x86_64/glibc).
A Pi needs that entire runtime — `libart.so`, `dex2oat64`, the boot image, `libandroid_runtime.so`,
plus acl's `libacl_graphics_jni.so` / `libsoundpool.so` and the framework jars — rebuilt for an
**arm64 Linux host** target. Everything else (Skia, Vulkan/dma-buf, Wayland, PipeWire, the OAT
cache) is available or tractable on a Pi; the ART lift is the critical path.

A `--vm=jvm` (ADR-0001 fallback, stock OpenJDK-arm64) path gets *something* running with no ART
build, at reduced fidelity — useful for early bring-up, not the end state (§4).

## 1. Board matrix

| | Pi 4 | Pi 5 | RK3588 (§7) |
|---|---|---|---|
| CPU | Cortex-A72 | Cortex-A76 | Cortex-A76+A55 (big.LITTLE) |
| GPU | VideoCore VI | VideoCore VII | Mali-G610 |
| Vulkan driver (Mesa) | V3DV (1.2) | V3DV (1.2/1.3) | Panfrost/**Panthor** (Bifrost/Valhall) or RADV-style Turnip N/A |
| Wayland | labwc/Wayfire (Bookworm) | labwc/Wayfire | vendor/Wayland stack |
| Audio | PipeWire (Bookworm) | PipeWire | PipeWire |
| RAM | 1–8 GB (use ≥4) | 4–16 GB | 4–32 GB |
| Relative perf for acl | tight but usable for media/CAL | comfortable | best of the three |
| Cross file | **new `cross/pi4.ini`** | reuse pi4 w/ `cpu=cortex-a76` | `cross/rk3588.ini` (exists) |

acl's sweet spot on any of these is the **automotive critical path** (media apps, Car App Library
template apps) which draw no Android Views — far more realistic on a Pi 4 than a heavy View/Compose
app. General View/Compose apps will run but be slow on Pi 4.

## 2. Phased milestones

Each phase is independently landable; later ones depend on earlier ones only where noted.

### P-A. Core cross-build (low risk, do first) — ✅ DONE (2026-07-15)
**Result:** a full arm64 `acl` **compiles, links, and runs** (under qemu-aarch64 emulation) in a
Debian **trixie** (g++ 14.2, full C++23) container with the stock arm64 dev packages — PipeWire,
FFmpeg/SoundPool, Wayland, xkbcommon, sdbus-c++/net, fmt, zlib all found; the JVM backend also
enabled (OpenJDK 21 arm64). Only non-code fix needed: install a JDK for `jni.h` (meson's `java
found: NO` otherwise; the 27 non-JNI objects compiled clean regardless). **The C++ core is
arm64-clean.** Skia + wcs gate OFF (not in the container), so gfx/wm build reduced — the real
graphics path is P-C. Reproduce: `podman run --platform linux/arm64 debian:trixie`, install the
P-A deps + `default-jdk-headless`, `meson setup b /src -Dtests=false && meson compile -C b`.

**Correctness (not just compilation):** `meson test` on arm64 → **17 Ok, 1 skipped, 1 Fail** — the
Tier A unit tests (DEX→JVM translator, pkg/manifest) and the integration tests all pass; the single
failure (`run-apk-default-framework`) is environmental — it wants a host-built `build/framework/classes`
absent in the fresh container (fails identically on x86 without that step), not an arm64 bug. So the
arm64 build is verified correct, not merely compilable.

**Pi-4 target compiler — characterized (Bookworm, arm64 container):** a full arm64 `acl` also
**builds and runs with stock Bookworm's g++ 12.2 and fmt 9.1** (rc=0) — so acl's C++23 usage does
NOT need g++13+. The ONE stock-Bookworm blocker is **meson 1.0.1 < the required 1.3.0**: install a
newer meson (`pip3 install --break-system-packages 'meson>=1.3'`, or bookworm-backports) and the
stock g++12 toolchain compiles acl cleanly. Net: **Pi 4 (Bookworm) can build acl from stock apt
packages + a pip meson** — no compiler upgrade required.

Original task (now satisfied by the above): confirm the acl C++ core compiles clean for arm64.
- Add `cross/pi4.ini` (copy `rk3588.ini`; `cpu = 'cortex-a72'`, point `sysroot` at a Pi OS
  Bookworm sysroot). A Pi 5 build reuses it with `cpu = 'cortex-a76'`.
- `meson setup build-pi4 --cross-file cross/pi4.ini` then `meson compile -C build-pi4`. The host-ART
  bits are runtime-loaded (`dlopen($ACL_ART_HOME/...)`), so the core links without an arm64 ART
  present; `--vm=null`/`--vm=jvm` codepaths and pkg (APK/zip/manifest), wm, gfx, svc all build.
- Sysroot needs the arm64 dev packages acl links: `wayland-client`, `libpipewire-0.3`, Vulkan
  loader/headers, `fmt`, `sdbus-c++` (optional), `libavcodec/format/util/swresample` (SoundPool is
  dlopen'd, but headers are needed to compile). **Exit criterion:** an arm64 `acl` binary that runs
  `acl` (version + module roster) on the Pi.
- **Gap:** verify no x86-only intrinsics/`-m` flags leaked into any module's `meson.build`
  (none expected — the code is portable C++23).

### P-B. Host ART for arm64-Linux (the critical path)
Produce an AOSP host runtime that runs *on the Pi* (arm64), mirroring today's glibc
`out/host/linux-x86`. **Feasibility spike (2026-07-15) — the shape is harder than first assumed:**

AOSP registers host toolchains for these OS/arch pairs only
(`build/soong/cc/config/*.go`, `registerToolchainFactory`):

| host libc | x86 / x86_64 | arm64 |
|---|---|---|
| glibc (`android.Linux`) | yes (acl uses this today) | **NO** |
| musl (`android.LinuxMusl`) | yes | **NO** |
| bionic (`android.LinuxBionic`) | yes | **yes — the only arm64 host** (`arm64_linux_host.go`) |

So the earlier `linux_musl_arm64` premise was wrong (musl host is x86-only), and there is **no stock
glibc arm64 host** either. The single arm64 Linux host is **`linux_bionic-arm64`** — Android's bionic
libc with a host-bionic linker. That collides with acl's core model: acl runs ART **in-process** with
the Pi's **glibc** Wayland / Vulkan / PipeWire libraries, and you cannot cleanly host two libcs
(bionic + glibc) in one process (loader, TLS, malloc, symbol namespaces). So P-B forks:

- **Path B1 — `linux_bionic-arm64` (stock, but fights the model).** Build ART bionic-arm64; then
  either (a) make the graphics/audio underside *also* bionic (build acl + Skia + Wayland/Vulkan/
  PipeWire clients against bionic — large, and the Pi's system libs are glibc), or (b) split ART into
  its own process and IPC to a glibc render/audio process (a big architectural change to the
  single-process design). Host Bionic is also a niche/fragile AOSP path.
- **Path B2 — add a glibc (or musl) arm64 host toolchain to Soong (clean end-state).** Register a
  `linux_glibc`/`linux_musl` × `arm64` `ToolchainFactory` (mirroring `x86_64_linux_host.go`), wire the
  aarch64 cross toolchain, and build ART for it. This yields an arm64 ART that coexists with the Pi's
  glibc libs exactly as the x86 host does today. Real Soong work, but it preserves the single-process
  architecture — **recommended** if pursued.

Once a target exists, the module set is the same either way: `libart`, `libartbase`, `libdexfile`,
`libprofile`, `dex2oat`, `libandroid_runtime`, `libnativehelper`; core boot jars (`core-oj`,
`core-libart`, `okhttp`, `bouncycastle`, `apache-xml`, `core-icu4j`, `conscrypt`);
`framework-minus-apex`/`framework-graphics`/`ext`/`telephony-common`; and acl's own
`libacl_graphics_jni.so` + `libsoundpool.so` rebuilt for it (the SoundPool FFmpeg-vs-OpenSSL dlopen
isolation matters *more* against a distro OpenSSL). `tools/run-apk.sh` already derives everything from
`ACL_ART_HOME`, so no harness change — only which tree you point it at.
- **Gaps:**
  - **libc coexistence (the crux):** bionic ART + glibc graphics/audio in one process — B1 or B2 above.
  - No stock glibc/musl arm64 host toolchain → B2 requires adding one to Soong first.
  - `dex2oat64` naming/ISA: the boot image path bakes `/x86_64/`; arm64 inserts `/arm64/` — verify
    `-Ximage` ISA-subdir handling (docs/art-host-spike.md notes the x86_64 case).
  - ART JIT on A72/A76: confirm the arm64 quick-compiler/JIT is enabled in the host build (host ART
    historically favored the interpreter).

**Toolchain spike done (2026-07-15) — the arm64 host-ART build path WORKS.** With
`HOST_CROSS_OS=linux_bionic HOST_CROSS_ARCH=arm64` (which lands in `soong.variables` as
`CrossHost=linux_bionic`/`CrossHostArch=arm64`), Soong instantiates the full bionic-arm64 host-cross
graph (~1M rules) and building the specific target
`out/soong/.intermediates/art/libdexfile/libdexfile/linux_bionic_arm64_shared/libdexfile.so` links
**`libartbase.so` + `libdexfile.so` for `linux_bionic`** — the artifact is a real `ELF 64-bit ...
AArch64` shared object. So the core ART libraries compile+link for arm64 host out of the box, no
source patches. (Note: a plain `m libdexfile` builds the Android *target* variant, not the host_cross —
you must request the `linux_bionic_arm64_shared` output explicitly.) **B1 is build-viable at the
toolchain level.** The remaining P-B risk is therefore NOT the toolchain but the bionic-vs-glibc
in-process coexistence (integration/architecture), plus scaling the build up to the full stack
(`libart`, `dex2oat`, `libandroid_runtime`, boot jars) and the boot image (P-D).
**Full native stack spike done (2026-07-15) — the whole ART host runtime builds for arm64.**
Building the `linux_bionic_arm64_shared` outputs of `libandroid_runtime` (+ `libart`,
`libnativehelper`, `libprofile`) links successfully: **`libart.so` (AArch64, ~104M unstripped) and
`libandroid_runtime.so` (AArch64, ~112M)** — exactly the two `.so`s acl `dlopen`s from
`$ACL_ART_HOME/lib64/` — plus everything `libandroid_runtime` transitively pulls (libart-compiler,
libsigchain, libbase, libc++, libsqlite, …), all as `linux_bionic` AArch64, **no source patches**.
So P-B is **build-complete at the native level**: the entire embedding surface acl depends on
compiles+links for an arm64 host. (Unstripped because the `linux_bionic` bp block disables strip for
crash symbols; stripped is far smaller.)
- **Sole remaining P-B risk = integration, not build:** bionic ART in-process with the Pi's glibc
  Wayland/Vulkan/PipeWire (B1 vs B2, above).
- **`dex2oats` (the boot-image compiler) also builds for arm64 (2026-07-15):** target
  `out/soong/.intermediates/art/dex2oat/dex2oats/linux_bionic_arm64/dex2oats` links as an AArch64 ELF
  (~198M unstripped). (Module name is `dex2oats`, not `dex2oat`, for the host-cross variant.) So ALL
  the core arm64 host-ART native binaries — `libart.so`, `libandroid_runtime.so`, `libnativehelper.so`,
  `libprofile.so`, `dex2oats` — build clean, no source patches. **The native side of a
  `linux_bionic-arm64` `ACL_ART_HOME` is build-complete.**
- **RUNTIME spike done (2026-07-15) — the arm64 ART executes.** The arm64 `dex2oats`
  (statically linked — host-bionic embeds the linker) runs under `qemu-aarch64-static`: it
  initialized the ART runtime and parsed its command line (printed usage from `dex2oat.cc:220`,
  exit 0). So the `linux_bionic-arm64` ART binaries are not just build-complete but **runnable**
  (at least under emulation — a Pi runs them natively). This is a strong signal the ART side works;
  it does NOT yet test the bionic-vs-glibc coexistence (dex2oats is all-bionic).
- **P-D boot image — finding:** AOSP does NOT auto-generate an arm64 *host* boot image (no
  `linux_bionic-arm64 boot.art` target in the Soong graph; host boot images are built only for the
  build host's x86_64). Options: run the arm64 `dex2oats` (under emulation) or the x86 `dex2oat`
  (cross-targeting arm64) over the boot classpath jars to emit `boot.art` manually — OR skip it: acl
  already defaults to imageless `-Xint` when `ACL_ART_BOOT_IMAGE` is unset, so a *first* arm64 boot
  needs no boot image (just slower). The boot jars are DEX/Java = arch-independent (reuse the x86 ones).
- **Then:** wire `ACL_ART_HOME` at the arm64 tree and boot it under acl's embedding seam — where the
  libc-coexistence question gets its empirical answer.

### P-C. GPU / Vulkan dma-buf bring-up
acl re-owns `android.graphics` on Skia + a Vulkan dma-buf present loop (ADR-0004). On a Pi that is
**Mesa V3DV**.
- Verify V3DV exposes what `graphics_jni`/`gpu_dmabuf.cpp` negotiates: `VK_EXT_external_memory_dma_buf`,
  `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_memory_fd`, and the modifiers acl advertises
  for `XRGB8888` (the logs show it querying compositor modifiers). V3DV supports external dma-buf
  import/export but its **modifier set is narrower** than desktop RADV — expect a format/modifier
  negotiation fallback path to be needed.
- Skia's Ganesh Vulkan backend on V3DV: builds fine; validate the tiled dma-buf export → Wayland
  `wl_buffer` path end-to-end (the `dmabuf present(A)` path in the logs).
- **Gaps:**
  - Modifier mismatch → blank/garbled frames: add a linear-modifier / `wl_shm` fallback for boards
    whose Vulkan modifiers don't intersect the compositor's.
  - `wp_linux_dmabuf` feedback on labwc/Wayfire vs the desktop compositors acl was developed against
    (Mutter/Sway) — Tier C matrix should add a Pi compositor.
  - Vulkan 1.2 feature floor: confirm nothing in graphics_jni assumes 1.3-only features.

### P-D. Boot image + OAT (perf)
Without AOT, ART runs `-Xint`/imageless (see the option in jvm_vm.cpp) — on a Cortex-A72 that is
painfully slow to start.
- Generate an arm64 boot image (`boot.art` for `arm64`) and an app OAT cache via `dex2oat64` on the
  arm64 host, matching `ACL_ART_BOOTCLASSPATH`.
- Wire `ACL_ART_BOOT_IMAGE` (already supported in jvm_vm.cpp) to the arm64 image.
- **Gap:** OAT cache location/permissions on a read-mostly Pi rootfs; and first-run dex2oat time.

### P-E. App corpus + conformance on-board
- Run the existing hash-pinned F-Droid corpus (the same apps validated on x86: opendrummer,
  pdfinspector, dialog fixtures) through `tools/run-apk.sh` on the Pi.
- Bring the Tier B integration harness onto the board; add a Pi entry to the Tier C compositor
  matrix (plan.md §5). The `run_*_art_test.py` harnesses already SKIP without `ACL_ART_HOME`, so
  they run unchanged once P-B lands.

## 2a. Pi bring-up runbook (hardware in hand)

Ordered so each milestone is independently demonstrable and risk rises last. Everything a Pi needs
that can be pre-built off-Pi already is (P-A/P-B done); the Pi work is native build + assembly + the
coexistence test.

**M1 — acl on the Pi via OpenJDK (`--vm=jvm`), low risk.** Proves the *desktop-side* stack on real
hardware without any bionic ART. **The reference Pi runs Debian/RPi-OS trixie — the exact toolchain
(g++ 14.2, meson ≥ 1.3) the off-Pi container build was validated against**, so no pip-meson
workaround and no compiler caveat (those were only for stock Bookworm). Run `tools/pi-build.sh`
(apt deps + `meson setup build && meson compile`), then `acl run app.apk --vm=jvm --present`.
Validates Wayland (labwc/Wayfire) + Mesa V3DV + PipeWire + input end-to-end on the Pi. Gaps that
surface here are P-C (V3DV dma-buf modifiers) and perf, not ART.

**M2 — stand up the arm64 host ART, boot it natively.** Off-Pi: `tools/assemble-arm64-art.sh`
produces a `linux_bionic-arm64` `ACL_ART_HOME` tree (37 libs + `dex2oats` + boot jars, ~37 MB
stripped) — scp it to the Pi. On the Pi: `export ACL_ART_HOME=<tree>` and confirm the bionic-arm64
ART boots natively (no qemu) — e.g. a static-`main` DEX under `acl run --vm=art` imageless (acl
defaults to `-Xint` with no `ACL_ART_BOOT_IMAGE`, so no boot image needed first). This retires
"does the arm64 ART run on real silicon" (we only proved it under emulation off-Pi).

**M3 — `--vm=art` full path: the bionic-vs-glibc coexistence crux.** Build `libacl_graphics_jni.so`
for `linux_bionic_arm64` (Soong, in the AOSP tree — watch for Skia-arm64 availability) + build acl
itself on the Pi (glibc). Run `acl run app.apk --vm=art --present`. This is the moment a **glibc**
acl process `dlopen`s the **bionic** `libart.so` and then a **glibc** Vulkan/Wayland/PipeWire lib —
the make-or-break test. Likely-OK because host-bionic libs carry their own bionic runtime (libart's
NEEDED set is all bionic `libc/libc++/...`), so the two libcs don't share symbols; the risk is the
JNI boundary (memory allocated on one side freed on the other) and TLS. If it holds → B1 is the
answer. If it fights → B2 (add a glibc/musl arm64 Soong toolchain so the whole process is one libc)
or split ART into its own process + IPC. **This is the one question no off-Pi spike can answer.**

## 3. What already works in acl's favor
- **arm64 is first-class**: cross files exist; the automotive targets (RK3588, SA8155P) are arm64.
- **Harness is host-agnostic**: `tools/run-apk.sh` + `ArtVm` derive everything from `ACL_ART_HOME`;
  no x86 is hardcoded in logic (only in which tree you point at).
- **Env-driven seams**: `ACL_ART_BOOTCLASSPATH`, `ACL_ART_BOOT_IMAGE`, `ACL_ART_LIBPATH`,
  `ACL_AUDIO`, `ACL_ART_GRAPHICS_LIB` — all board-portable.
- **Deps are stock on Pi OS Bookworm**: Wayland (labwc/Wayfire), PipeWire, Mesa/Vulkan, glibc.

## 4. Fallback: `--vm=jvm` on OpenJDK-arm64 (bring-up only)
The ADR-0001 fallback runs the DEX→JVM-translated classes on a stock OpenJDK (arm64 exists on Pi
OS). This sidesteps all of P-B. Use it to validate P-A/P-C plumbing (windowing, input, audio,
graphics) before the ART build exists. Fidelity loss: the `dalvik.system` shim, no real
framework-native floor, and the DEX→JVM translator's remaining unsupported-opcode tail. **Not the
product** — a scaffold.

## 5. Effort / risk summary
| Phase | Effort | Risk | Blocks |
|---|---|---|---|
| P-A core cross-build | low | low | nothing |
| P-B arm64 host ART | **high** | **high** (musl/glibc, Soong host target) | the real product |
| P-C Vulkan/V3DV | medium | medium (modifiers) | rendering |
| P-D boot image/OAT | medium | low | usable perf |
| P-E corpus/conformance | low | low | — |

The program is gated on **P-B**. Recommend doing **P-A now** (de-risks the core, cheap), then a
spike on P-B (can a `linux_musl_arm64` ART even be built + booted with acl's embedding seam?) before
committing to the full board bring-up.

## 6. Pi 4 vs Pi 5
Same ART work either way. Pi 5 is ~2–3× faster (A76), a more capable V3DV (VideoCore VII), and more
RAM/thermal headroom — it removes most of the interactive-perf pain. If the goal is a *usable*
demo, prefer Pi 5; if the goal is proving the floor on the weakest realistic target, Pi 4 (≥4 GB).

## 7. RK3588 support — gaps

`cross/rk3588.ini` exists (aarch64, `cpu=cortex-a76`, Mali-G610), but it is a **template** and the
board has never been brought up. Gaps, beyond the shared arm64 work (P-A/P-B/P-D above which apply
identically):

1. **Toolchain/sysroot are placeholders.** `sysroot = '/opt/sysroots/rk3588'` and the
   `aarch64-linux-gnu-` prefix are stubs — no real sysroot is wired, and CI does not build the
   cross target. **Gap:** provision a real RK3588 sysroot (Debian/Ubuntu arm64 or the vendor BSP),
   populate the dev packages P-A lists, and add a CI job that at least *configures + compiles* the
   cross build (even if it can't run).
2. **GPU stack choice is unresolved.** The cross file says "Mali-G610 / Panthor+RADV target" — but
   that pairing needs pinning: **Panthor** (the upstream Valhall/G610 kernel DRM driver) +
   **Panfrost/PanVk** (Mesa Vulkan for Mali) is the mainline path; the vendor **Mali blob** (proprietary)
   is the other. **Gaps:** (a) decide mainline Mesa PanVk vs vendor blob; (b) PanVk Vulkan maturity
   on G610 is behind V3DV/RADV — the dma-buf modifier + external-memory extensions in P-C are the
   risk; (c) if using the vendor blob, license/namespace isolation (it is not in-tree, must be
   dlopen'd like the graphics lib).  Note "RADV" in the cross-file comment is a misnomer for Mali —
   RADV is AMD; the Mali Vulkan driver is PanVk. **Fix the comment.**
3. **Kernel/DRM + Wayland compositor.** RK3588 boards vary (Rockchip BSP kernel vs mainline). acl
   needs `wp_linux_dmabuf`, `wp_presentation`, `wp_fractional_scale_v1`, `wp_viewport` (ADR-0006).
   **Gap:** confirm the board's compositor advertises these; the Rockchip vendor stack may lag
   mainline protocol support.
4. **Automotive profile is the intended use.** RK3588/SA8155P are automotive targets; the default
   `profile_automotive=ON` build pulls `svc/car` + `hosts/{cal,media}`. **Gap:** the vehicle-property
   hub (`svc/car`) and the CAL/media hosts have no board backend yet — they are host-desktop stubs.
   Real board bring-up needs the automotive HAL/vsomeip (MPL-2.0, allowed) wiring, which is a
   separate track from graphics.
5. **No boot image for arm64 yet** (shared with P-D) — same OAT/`-Ximage` ISA-subdir work.
6. **SA8155P parallel gap.** `cross/sa8155p.ini` (Adreno / Freedreno+**Turnip**) has the same
   template/sysroot/CI gaps; Turnip (Mesa Vulkan for Adreno) is more mature than PanVk, so SA8155P's
   *GPU* risk is lower than RK3588's, but its BSP/kernel access is typically more locked down.

### RK3588 quick checklist
- [ ] Real sysroot + toolchain wired into `cross/rk3588.ini`; CI cross-configure/compile job.
- [ ] Decide + document GPU driver (PanVk mainline vs vendor blob); fix the "RADV" comment.
- [ ] Verify compositor protocols (dma-buf/presentation/fractional-scale/viewport).
- [ ] P-B/P-D arm64 host ART + boot image (shared with Pi).
- [ ] P-C dma-buf modifier negotiation validated on PanVk (add linear/shm fallback).
- [ ] Automotive backends: `svc/car` + CAL/media host board wiring (separate track).

## 8. First concrete step
`cross/pi4.ini` + a P-A cross-compile smoke build. It is cheap, needs no ART, and proves the core is
arm64-clean — de-risking every later phase. Then spike P-B (build + boot a `linux_musl_arm64` ART
under acl's embedding seam) before committing to full board bring-up.

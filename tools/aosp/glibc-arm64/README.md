<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l build tooling. Patches AOSP Soong, make, ART and libcutils (Apache-2.0)
     and builds LLVM compiler-rt (Apache-2.0 with LLVM exception). No GPL-3.0 sources. -->

# Host ART for arm64 glibc

Stock AOSP builds host ART for arm64 against bionic only, and a glibc process cannot load that
`libart.so` (`docs/pi-plan.md`, P-B). This directory adds a `linux_glibc` x `arm64` host target to
the Android 13 tree, so ART builds against glibc and loads into the a4l process on arm64 the way
it does on x86-64.

Result on 2026-11-16, android-13.0.0_r84, on an SA8155P board (glibc 2.31):

| Step | Result |
|---|---|
| `dalvikvm64` on the board, static-main class | prints, exit 0 |
| `a4l run hello.apk --vm=art`, ART in the a4l process | prints, exit 0 |
| A Java-only F-Droid app on the real framework, headless | Activity resumed, decor laid out, exit 0, 1.3 s |
| The same app with `--present` and `ACL_PRESENT_GPU=1`, as an ivi surface | on screen, 1280x720, first frame 1.35 s after process start |

The last two rows load `libandroid_runtime.so`, `libacl_graphics_jni.so` (with Skia) and
`libacl_gles_jni.so`: 28 of 28 framework native groups registered. The frame is a dma-buf
rendered by Skia on the board's Vulkan driver (Adreno, Vulkan 1.1) and handed to Weston 8 through
`zwp_linux_dmabuf_v1`; a screen dump showed the app's clock face. `libart.so` needs glibc 2.29 or
later.

## Use

```sh
export ACL_AOSP_ROOT=/mnt/dev/aosp
export ACL_ARM64_SYSROOT=/path/to/arm64-sysroot   # Debian multiarch layout, with GCC's crt and libgcc
export LLVM_SRC=/path/to/llvm-project             # at llvmorg-14.0.6

tools/aosp/glibc-arm64/build-runtimes.sh          # once; three static libraries
tools/aosp/glibc-arm64/apply.sh                   # patch the tree; --revert undoes it

cd $ACL_AOSP_ROOT && source build/envsetup.sh && lunch aosp_x86_64-eng
I=out/soong/.intermediates
HOST_CROSS_OS=linux_glibc HOST_CROSS_ARCH=arm64 ALLOW_MISSING_DEPENDENCIES=true \
  m --skip-soong-tests --soong-only \
  $I/art/runtime/libart/linux_glibc_arm64_shared/libart.so \
  $I/art/dalvikvm/dalvikvm/linux_glibc_arm64/dalvikvm64 \
  $I/art/openjdkjvm/libopenjdkjvm/linux_glibc_arm64_shared/libopenjdkjvm.so \
  $I/libcore/libjavacore/linux_glibc_arm64_shared/libjavacore.so \
  $I/libcore/libopenjdk/linux_glibc_arm64_shared/libopenjdk.so \
  $I/libcore/libandroidio/linux_glibc_arm64_shared/libandroidio.so \
  $I/libnativehelper/libnativehelper/linux_glibc_arm64_shared/libnativehelper.so \
  $I/external/icu/android_icu4j/libcore_bridge/src/native/libicu_jni/linux_glibc_arm64_shared/libicu_jni.so

ACL_ART_LIBC=glibc ACL_ART_STRIP=1 tools/assemble-arm64-art.sh /tmp/a4l-arm64/art
```

Copy the tree to the target and run with `ACL_ART_HOME=<tree>` and
`LD_LIBRARY_PATH=<tree>/lib64`. The boot jars are DEX and come from the x86 host build
(`m build-art-host`), which has to exist first. The tree is 29 libraries, 25 MB stripped, before
the framework natives.

### An app on the real framework

Build five more outputs the same way, with the a4l JNI modules in `vendor/acl` as on x86-64
(`tools/aosp/graphics_jni/README.md`), and assemble again:

```sh
  $I/frameworks/base/core/jni/libandroid_runtime/linux_glibc_arm64_shared/libandroid_runtime.so \
  $I/vendor/acl/graphics_jni/libacl_graphics_jni/linux_glibc_arm64_shared/libacl_graphics_jni.so \
  $I/vendor/acl/gles_jni/libacl_gles_jni/linux_glibc_arm64_shared/libacl_gles_jni.so \
  $I/art/compiler/libart-compiler/linux_glibc_arm64_shared/libart-compiler.so \
  $I/external/conscrypt/libjavacrypto/linux_glibc_arm64_shared/libjavacrypto.so
```

The last two are loaded by name and appear in no `NEEDED` list. Without `libjavacrypto.so` no
`java.security` provider loads and the framework's system `AssetManager` fails on its first
digest; without `libart-compiler.so` ART runs interpreted. The tree is then 50 libraries, 54 MB
stripped.

```sh
tools/stage-arm64-framework.sh /tmp/a4l-arm64/fw   # boot jars, a4l's dex jars, framework-res.apk
# copy /tmp/a4l-arm64 and an arm64 a4l to the target as R/{art,fw,a4l}, then there:
sh R/fw/run.sh R app.apk
```

To show a window, a4l itself needs Skia and Vulkan in the cross build:

```sh
SKIA_SRC=/path/to/skia tools/aosp/glibc-arm64/build-skia.sh /path/to/skia-arm64
meson setup build-arm64 --cross-file <cross file> -Dskia_prefix=/path/to/skia-arm64
# on the target, under ivi-shell; a controller has to put the surface on a layer
ACL_PRESENT_GPU=1 ACL_WL_SHELL=ivi ACL_IVI_SURFACE_ID=9000 sh R/fw/run.sh R app.apk --present
```

`build-skia.sh` builds the Skia pinned in `third_party/skia/METADATA` with the x86-64 GN
arguments plus the cross toolchain: 50 s on 32 threads, 20 archives. The cross build needs
`vulkan.hpp` from the same Vulkan-Headers release as `vulkan_core.h`; the headers are
architecture-independent, so the build machine's can be put ahead of the sysroot's.

`stage-arm64-framework.sh` copies 80 MB of dex and data from the x86-64 build; `run.sh` sets
what `tools/run-apk.sh` sets on x86-64, without the sandbox, the bionic runtime, the CA store
or WebView.

Three flags in the build line carry weight:

- `ALLOW_MISSING_DEPENDENCIES=true`. A new host target makes Soong analyze every host module
  for it, and modules that cannot support it (Rust, some tools) would stop analysis. With the
  flag they become errors only if something asks for them, and ART does not.
- `--soong-only`. The make stage does not know the new target and fails on it. Outputs are
  named by path, so make is not needed.
- `--skip-soong-tests`. The Soong changes fail Soong's own golden tests.

Revert the tree (`apply.sh --revert`) before a normal x86 build for the same reason.

## What the patches change

| Repository | Change | Why |
|---|---|---|
| `build/soong` | `Arm64` added to the `linux_glibc` OS; `arm64_linux_glibc_host.go` | the toolchain: clang `aarch64-linux-gnu`, sysroot and GCC crt from `ACL_ARM64_SYSROOT` |
| `build/soong` | `bazel/configurability.go` entry | Soong panics on an unknown OS and arch pair |
| `build/soong` | Rust modules disabled for the target | there is no Rust toolchain for it |
| `build/make` | `HOST_CROSS_OS=linux_glibc` accepted | stock allows only `linux_bionic` and `windows` |
| `prebuilts/clang` | compiler-rt mapped for `glibc_arm64`, only where the file exists | the prebuilt ships none for `aarch64-linux-gnu` |
| `art/build` | host-cross variants skipped in the testcases list | host and host-cross now share an OS, and both claimed `lib64/libvixld.so` |
| `art` | `ucontext_t`, `<signal.h>`, `kPageSize`, `SEGV_MTEAERR` | glibc after 2.26 has no `struct ucontext`; arm64 glibc has no `PAGE_SIZE` |
| `art` | worker stack raised to `PTHREAD_STACK_MIN` | 128 KB on arm64 glibc; ART asks for 64 KB and `pthread_attr_setstack` fails |
| `system/core` | `gettid` guard moved from glibc 2.32 to 2.30 | glibc declares `gettid` since 2.30 |

`overlay/include/sys/capability.h` declares `capget` and `capset` for sysroots without libcap's
headers; libcore includes it.

The compiler-rt libraries are builtins, profile and the minimal UBSan runtime. Soong links the
first and third into sanitized host modules. libc++ is not in the list: on glibc hosts Soong
builds it from `external/libcxx`, and unwinding uses the sysroot's libgcc.

## Relation to the musl attempt

`tools/aosp/b2-musl-arm64` tried the same thing against musl in July 2026 and stopped in the
host module graph, before `libbase` linked. Two things differ here. `ALLOW_MISSING_DEPENDENCIES`
removes that graph work. And a glibc host needs three small runtime libraries where musl needed
the whole LLVM runtime set. A musl ART would also not serve a system whose GPU drivers are
glibc, which is the case this was built for.

## Limits

- One app was run, and it has no native libraries. The bionic loader that runs an app's own
  `.so` files is x86-64 only.
- Presentation on the board needs the 4-buffer pool. With 3 it ran at half the refresh rate
  (present interval p50 31.3 ms on a 16.7 ms display): Weston 8 released a replaced buffer
  about 29 ms after the commit that replaced it, so all three were held when the next frame
  needed one. With 4 the interval is p50 16.7 ms, p99 18.8 ms over 360 presents.
- The presenter takes the linear modifier by default. The Adreno 640 driver lists the vendor
  modifier `0x0500000000000001` for `R8G8B8A8` only (LINEAR alone for `B8G8R8A8`), so it
  needs ABGR8888, which `ACL_PRESENT_ALLOW_ABGR=1` allows. With it a4l presented ABGR8888
  with that modifier at p50 16.7 ms, p99 18.0 ms over 1680 presents, and the picture was
  scrambled: on Weston's screen capture in two runs out of two, and on the panel itself.
  The driver's buffer is a compressed layout (3,710,976 bytes against 3,686,400 linear at
  1280x720). The likely cause: this vendor stack keeps part of a compressed buffer's
  state in a metadata buffer beside it, which its EGL passes to the compositor over a
  Wayland protocol of its own (`EGL_WL_bind_wayland_display`) and linux-dmabuf does not
  carry.
- `ACL_PRESENT_SWAPCHAIN=1` presents through a Vulkan swapchain on the toplevel
  (`VK_KHR_wayland_surface`), so the driver allocates the window's buffers and sends them
  over that vendor protocol, each with its metadata buffer. On the board: picture correct,
  p50 16.7 ms, p99 18.4 ms over 960 presents, and 415 of 417 presentation feedbacks
  flagged zero-copy, where the dma-buf path gets every feedback discarded. The four
  buffers the driver made were 3,686,400 bytes each, the linear size, so it did not
  compress them, and nothing found makes it: a probe that created swapchains in
  `R8G8B8A8` and `B8G8R8A8` with four usage combinations got 3,686,400-byte buffers in
  all eight, and the driver's only related switch (`DISABLE_UBWC_VK`) turns compression
  off. The same driver's EGL does compress a window's buffers: a 1280x720 EGL window got
  3,710,976 bytes, the compressed size. So compressed window buffers on this board need
  an EGL window, which a4l's Skia path does not present through. The swapchain is MAILBOX where the driver has it (it does here: p50
  16.6 ms, p99 21.6 ms over 360 presents), since a4l paces frames itself. Its composite
  alpha on the board is opaque, so a video hole would not show through; no video was
  run there. On x86-64 (Mesa) a resize, video on a subsurface and WebView all work in
  this mode.
- Input was not tried on the board.
- The cross-built a4l had no libcrypto, so it did not check the APK's signature.

- One sysroot was used (Debian bullseye, glibc 2.31, GCC 10). The toolchain file names GCC 10.
- The boot image is not built; ART runs without one and logs that it could not find it.
- `dex2oat` for the target was not built or tried.
- Nothing here runs in CI.

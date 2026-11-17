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
| `libandroid_runtime.so`, `libacl_graphics_jni.so` (with Skia), `libacl_gles_jni.so` | build; not run |

No app with an Activity has been run on this ART yet. `libart.so` needs glibc 2.29 or later.

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

ACL_ART_LIBC=glibc ACL_ART_STRIP=1 tools/assemble-arm64-art.sh /tmp/art-glibc-arm64
```

Copy the tree to the target and run with `ACL_ART_HOME=<tree>` and
`LD_LIBRARY_PATH=<tree>/lib64`. The boot jars are DEX and come from the x86 host build
(`m build-art-host`), which has to exist first. The tree is 29 libraries, 25 MB stripped, before
the framework natives.

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

- One sysroot was used (Debian bullseye, glibc 2.31, GCC 10). The toolchain file names GCC 10.
- The boot image is not built; ART runs without one and logs that it could not find it.
- `dex2oat` for the target was not built or tried.
- Nothing here runs in CI.

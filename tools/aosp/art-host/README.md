<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l build tooling for the split-process ART design (docs/split-process-art.md).
     Consumes AOSP prebuilts + the stock linux_bionic host toolchain. No GPL-3.0 sources. -->

# Building `acl-art-host` for the Pi (linux_bionic-arm64)

`acl-art-host` (src/runtime/art_host_main.cpp + src/ipc/*) is the bionic side of the arm64 split-process
design: it `dlopen`s the **stock** bionic host ART (`libart.so`) and runs the app, driven by the glibc
`acl` process over `acl_ipc`. Being bionic itself, it boots `libart.so` cleanly — **no two-libc SIGTRAP**
(the wall the in-process glibc `acl` hit; docs/pi-plan.md §P-B/M3). This needs **no B2 patch and no source
changes** — it's the stock `linux_bionic-arm64` host toolchain that already builds ART.

## Build

Copy the sources into the AOSP tree and build the host-cross variant:

```sh
mkdir -p $AOSP/external/acl-art-host/ipc
cp src/runtime/art_host_main.cpp $AOSP/external/acl-art-host/art_host_main.cpp
cp src/ipc/ipc.{h,cpp}           $AOSP/external/acl-art-host/ipc/
cp tools/aosp/art-host/Android.bp $AOSP/external/acl-art-host/Android.bp

cd $AOSP && source build/envsetup.sh && lunch aosp_x86_64-eng
HOST_CROSS_OS=linux_bionic HOST_CROSS_ARCH=arm64 m \
  out/soong/.intermediates/external/acl-art-host/acl-art-host/linux_bionic_arm64/acl-art-host
```

The result is a ~1 MB AArch64 bionic PIE (embedded linker); its NEEDED set (`libc++/libc/libm/libdl.so`)
is all in the assembled `ACL_ART_HOME` tree's `lib64`.

## The ART runtime tree also needs the core native libs

`ArtVm`/`assemble-arm64-art.sh` pulls the transitive NEEDED closure of `libart.so`, but ART loads more
native libs at runtime via `System.loadLibrary` in `Runtime::InitNativeMethods()` — **not** in the NEEDED
closure. Build these for `linux_bionic_arm64` and add them (plus their own NEEDED closure) to the tree's
`lib64`: `libicu_jni`, `libjavacore`, `libopenjdk`, `libandroidio`, `libnativehelper` (and whatever their
`dlopen` chain pulls). `assemble-arm64-art.sh` should be extended to include them.

## Run on the Pi

```sh
export ACL_ART_HOME=/tmp/acl-art-arm64  ACL_ART_HOST=/tmp/acl-art-host
export ACL_ART_SPLIT_VMLIB=$ACL_ART_HOME/lib64/libart.so
export LD_LIBRARY_PATH=$ACL_ART_HOME/lib64:/usr/lib/jvm/java-21-openjdk-arm64/lib/server
acl run app.apk --vm=art-split --main=<Class>
```

`acl` (glibc) `posix_spawn`s `acl-art-host` (bionic) with an inherited SEQPACKET fd; art-host boots the
bionic ART and streams output back. `LD_LIBRARY_PATH` carries the tree's bionic libs (for art-host) and
the JDK's `libjvm.so` (a4l's hard NEEDED) — no soname collision (bionic `libc.so` vs glibc `libc.so.6`).

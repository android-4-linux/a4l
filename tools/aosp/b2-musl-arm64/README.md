<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original acl build tooling. Consumes/patches AOSP Soong (Apache-2.0). No GPL-3.0 sources. -->

# B2: a musl-arm64 host ART toolchain for Soong

## Why

acl runs the app's DEX on a real AOSP **host** ART, loaded in-process (`dlopen`) alongside the
platform's glibc Wayland / Vulkan / PipeWire libraries. On x86 that host ART is `linux_glibc` — same
libc as the rest of the process, no conflict. On **arm64 the only stock AOSP host is `linux_bionic`**,
and a glibc `acl` process **cannot `dlopen` a bionic `libart.so`**: bionic `libc` cannot cold-load
under a foreign (glibc) loader — it SIGTRAPs at load (proven on a Pi 4; see `docs/pi-plan.md` P-B /
§2a M3). Two libcs cannot coexist that way.

**B2** removes the conflict by building the arm64 host ART against **musl** — a single, self-contained
libc that links only its own runtime, coexisting with the platform glibc exactly as the x86 host does.
musl is the right choice over glibc-arm64 because AOSP builds `external/musl` **from source** and its
aarch64 port is complete, so **no prebuilt arm64 sysroot is needed** (glibc-arm64's blocker).

## The patch (`soong-musl-arm64.patch`) — four sites

1. **`build/soong/android/arch.go`** — add `Arm64` to the `LinuxMusl` OsType arch list
   (stock: `X86, X86_64` only; that gap is the sole reason Soong never instantiated a musl-arm64
   variant, even though `external/musl` fully declares aarch64).
2. **`build/soong/cc/config/arm64_musl_host.go`** (new) — register a `LinuxMusl × Arm64` toolchain
   factory. Composes `toolchainArm64` (arm64 arch flags) with the arch-agnostic `toolchainMusl`
   mixin, ClangTriple `aarch64-linux-musl`. Because the only arm64 base (`toolchainArm64`) embeds
   `toolchainBionic`, the factory overrides the inherited bionic-ness — `Bionic()→false`, the six
   `Crt*` objects, and `DefaultSharedLibraries` → musl.
3. **`build/make/core/envsetup.mk`** — accept `HOST_CROSS_OS=linux_musl` (stock gates the host-cross
   OS to `linux_bionic`/`windows`). The existing `HOST_CROSS_ARCH=arm64` stanza in
   `android_soong_config_vars.mk` already forces build-from-source, so no change there.

## Apply + build

```sh
cd $AOSP_ROOT
git -C build/soong apply  .../soong-musl-arm64.patch   # or apply the three hunks by hand
git -C build/make  apply  .../soong-musl-arm64.patch
source build/envsetup.sh && lunch aosp_x86_64-eng
# build the arm64 musl host ART libs (host-cross variant → explicit output path):
HOST_CROSS_OS=linux_musl HOST_CROSS_ARCH=arm64 m \
  out/soong/.intermediates/external/musl/libc_musl/linux_musl_arm64_shared/libc_musl.so \
  out/soong/.intermediates/art/runtime/libart/linux_musl_arm64_shared/libart.so
```

Then assemble an `ACL_ART_HOME` from the `linux_musl_arm64` intermediates (adapt
`tools/assemble-arm64-art.sh`, swapping the `linux_bionic_arm64` glob for `linux_musl_arm64`) and run
`acl run app.apk --vm=art` on the Pi — this time the `dlopen` lands in a single-libc process.

## Status — spiked on the Android-13 tree (2026-07-15)

`HOST_CROSS_OS=linux_musl HOST_CROSS_ARCH=arm64 m --skip-soong-tests nothing` was driven through the
Soong integration layers; each failure was a mechanical registration gap (captured as the five sites
above), not a design problem:

1. Go compile — clean (`go vet` on the changed packages: no ambiguous-method errors; the
   `toolchainArm64 + toolchainMusl` composition with the bionic-ness overrides is correct).
2. `LinuxMusl` was X86-only → added `Arm64` (arch.go).
3. `HOST_CROSS_OS` gated to bionic/windows → added `linux_musl` (envsetup.mk).
4. bp2build panic `Unknown os+arch: linux_musl_arm64` → registered in configurability.go.
5. `Toolchain not found for linux_musl arch "arm64"` → the factory wasn't in the soong-cc-config
   srcs list (explicit, not a glob) → added to Android.bp.

**Result: the toolchain works.** Soong finds `linux_musl_arm64` and instantiates musl-arm64 variants
of `external/musl` (`libc_musl_crtbegin_*`, `libc_musl_crtend`, …).

**The one remaining external dependency — the true B2 boundary:** those musl crt modules depend on
compiler-rt's `clang_rt.crtbegin`/`clang_rt.crtend`, and **the clang prebuilt ships no aarch64
*Linux host* compiler-rt** — only `x86_64/i686-linux-musl` host objects and `aarch64-android`
(bionic) / baremetal builtins exist. There is no `aarch64-linux-musl` crtbegin/crtend/builtins,
because AOSP's only stock arm64 host is bionic (which uses its own crt), so nobody needed it.

**Next step to finish B2:** provide aarch64-linux-musl host compiler-rt. Either (a) build compiler-rt
from `toolchain/llvm-project` for the `aarch64-linux-musl` target (not synced in this tree — needs a
sync or a standalone compiler-rt build), or (b) pragmatically retarget the musl-arm64 toolchain at the
existing `libclang_rt.builtins-aarch64-android.a` (builtins are libc-agnostic soft-float/overflow
helpers) and generate the two tiny crtbegin/crtend objects. After that, ART's own libs
(`libart.so`, `libandroid_runtime.so`) build on the toolchain, and `tools/assemble-arm64-art.sh`
(swap the `linux_bionic_arm64` glob for `linux_musl_arm64`) yields a single-libc `ACL_ART_HOME` that
`acl run --vm=art` can `dlopen` on the Pi without the two-libc SIGTRAP.

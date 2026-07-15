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

**The remaining external dependency — the true B2 boundary (fully characterized 2026-07-15):** those
musl crt modules pull `clang_rt.crtbegin`/`crtend`, and more generally **the clang prebuilt ships the
entire LLVM runtime set only for the shipped host triples.** Per-target file counts under
`clang-r450784d/.../lib/linux/<triple>/`:

| host triple | runtime files |
|---|---|
| `x86_64-linux-musl` | **90** (crtbegin/crtend, builtins, libc++, libc++abi, libunwind) |
| `aarch64-linux-musl` | **0 — absent** |
| `aarch64-linux-gnu`  | **0 — absent** (so glibc-arm64 host has the same gap) |

`toolchain/llvm-project` is **not synced**, so the source to build them isn't in the tree either. The
`aarch64/libunwind.a` that does exist is the *android/generic* aarch64 build, not the
`aarch64-linux-musl` host triple Soong resolves against. So this is **not** "generate two crt objects"
— it is **build the whole LLVM runtime set for the `aarch64-linux-musl` host**. No in-tree shortcut.

**Next step to finish B2:** sync `toolchain/llvm-project` and build the LLVM runtimes for
`aarch64-linux-musl` (the same way AOSP's toolchain team produces the `x86_64-linux-musl` set — a
compiler-rt + libcxx/libcxxabi/libunwind runtimes build), then drop them under the
`clang-r450784d/.../aarch64-linux-musl/` path the toolchain resolves. After that, ART's own libs
(`libart.so`, `libandroid_runtime.so`) build on the toolchain, and `tools/assemble-arm64-art.sh`
(swap the `linux_bionic_arm64` glob for `linux_musl_arm64`) yields a single-libc `ACL_ART_HOME` that
`acl run --vm=art` can `dlopen` on the Pi without the two-libc SIGTRAP.

**Strategic fork this exposes:** B2's remaining cost is an **LLVM-runtime port** (llvm sync + a
runtimes cross-build for a host triple no one has shipped). Meanwhile the **bionic** arm64 ART
*already builds and runs natively on the Pi* (P-B / M2). So the honest comparison is now: finish the
B2 toolchain (clean single-process end-state, but pay the LLVM-runtime port) **vs.** a
**split-process design** — run the already-working bionic ART in its own all-bionic process and IPC
to the glibc render/audio process (no toolchain port; pay an IPC/architecture layer instead). Pick
per how much the single-process model is worth against the LLVM-runtime-port cost.

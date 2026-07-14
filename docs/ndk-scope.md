<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: Android NDK stable API docs (libandroid/ANativeWindow/ALooper), AOSP bionic
     (BSD) + linker docs, libhybris/Anbox (Apache-2.0, per-file license check). No GPL-3.0 sources. -->

# NDK / native libraries — scope and status

`plan.md` ADR-5 stages NDK support to Phase 4. This tracks the increments.

## The chain

An APK ships native code as `lib/<abi>/*.so` (ELF, linked against **bionic** + the NDK stable
APIs). `System.loadLibrary("foo")` → the classloader's native search path → `lib<foo>.so` →
`dlopen` → its `JNI_OnLoad` runs and `RegisterNatives` binds the app's `native` methods. Running
these on a glibc/Wayland host breaks into: (1) find/extract the right ABI, (2) CPU architecture,
(3) the bionic ABI bridge, (4) the NDK trampolines. See the fuller write-up in the project notes.

## DONE — front of the chain: ABI selection + search path

- **`pkg`**: `Apk::host_abi_candidates()` gives this build's ABIs in Android's preference order
  (x86_64 → `{x86_64, x86}`, arm64 → `{arm64-v8a, armeabi-v7a, armeabi}`); `Apk::select_abi(apkAbis)`
  returns the first host-compatible ABI present, else "".
- **`acl run`**: when the APK has native libs, extract `lib/<abi>/*.so` for the selected ABI to a
  per-app dir (`$XDG_CACHE_HOME/acl/<pkg>/lib`) and add `-Djava.library.path=<dir>` so host ART's
  `System.loadLibrary` resolves them. An ABI mismatch (e.g. an arm-only app on an x86_64 host) is
  reported, not extracted.

**Verified end-to-end** with a host-built x86_64 JNI fixture (`libacldemo.so` with a `JNI_OnLoad`
registering `answer()`; a `NativeMain` that `System.loadLibrary("acldemo")` then calls it): acl
selects `x86_64`, extracts the lib, and the app logs `loaded libacldemo; answer()=42` — so
`loadLibrary → dlopen → JNI_OnLoad → RegisterNatives → native call` all work on host ART.

This turns the old opaque `UnsatisfiedLinkError: lib*.so: cannot open shared object file` into
either a successful load (host-compatible `.so`) or a precise ABI-mismatch report.

## NEXT — why real apps still don't load their native code

The corpus apps that hit this ship **arm-only** (Spotify `libstub.so`: arm64/armeabi) or **arm +
x86-32** (Rive `librive.so`) — no x86_64 — so on an x86_64 host they fail on **architecture** even
with the search path wired. Two hard pieces remain (Phase 4 / R-6):

1. **Bionic ABI bridge** — app `.so` are linked against **bionic**, not glibc (different libc /
   `pthread`/TLS layout / ifuncs). Build AOSP `bionic` + `linker64` for the host and load app `.so`
   in a **bionic linker namespace**, bridging only at the NDK-API boundary. The tractable target is
   an **arm64 host** (the automotive boards) where no CPU translation is needed; on x86_64 desktop
   this additionally needs arm→x86_64 translation (QEMU user / houdini-class) or x86_64-native apps.
2. **NDK trampolines** (`src/ndk/`) — resolve the `.so`'s imports to acl: `libandroid`
   (`ANativeWindow` → our dma-buf/Wayland surface, `ALooper` → the reactor, `AAssetManager` → `pkg`,
   `AChoreographer`), `liblog`, `libEGL`/`libGLESv3`/`libvulkan` → host Mesa, `libaaudio`/
   `libOpenSLES` → PipeWire, `libjnigraphics` → Skia. `NativeActivity` apps need this with no View
   tree; JNI-augmented Java apps need it plus what already works.

The front of the chain (this change) is what every later step builds on.

## Verify

```sh
# host-.so fixture (scratchpad): proves the loadLibrary plumbing
acl run ndkdemo.apk --main=com.acl.ndkdemo.NativeMain   # -> "loaded libacldemo; answer()=42"
# a real arm-only app reports the mismatch:
acl run <arm-only>.apk                                  # -> "no host-compatible ABI ... bionic bridge"
```

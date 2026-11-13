<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: a4l P2.4/V3 build tooling; consumes external/skia (Apache-2.0). No GPL-3.0 sources. -->

# libacl_graphics_jni — a4l's android.graphics natives (P2.4/V3)

a4l-owned minimal `android.graphics` JNI (`Bitmap`/`Canvas`/`Paint`/`ColorSpace$Rgb`) implemented
as thin natives over **core Skia**, so the real framework graphics classes render on **Android's
own host `libskia`** on host ART — the ADR-4 "re-own android.graphics on our Skia" seam.

## Why an a4l-owned lib (not the host libandroid_runtime graphics)

The host `libandroid_runtime` is the **layoutlib** build: it compiles `@CriticalNative` natives with
the normal `(JNIEnv*, jclass, …)` signature (`graphics_jni_helpers.h`: *"Host targets (layoutlib) do
not differentiate between regular and critical native methods"*). But real host ART **honors**
`@CriticalNative` (calls with the critical, handle-only convention) → ABI mismatch → crash. Rather
than clear/patch `@CriticalNative` or rebuild `libhwui`, a4l compiles its **own** natives with the
correct signatures: the `@CriticalNative` methods (`nativeIsImmutable`, `Paint.nSet*`) take just the
handle; normal/`@FastNative` ones keep `(env, clazz, …)`. Matches the annotation, no AOSP change.

## Build (Soong)

Copied into the AOSP tree at **`$AOSP/vendor/acl/graphics_jni/`**, then:

```sh
lunch aosp_x86_64-eng
m libacl_graphics_jni     # -> out/host/linux-x86/lib64/libacl_graphics_jni.so
```

It links Android's host `libskia` + the exact dep closure (webp/sfntly/wuffs static; png/jpeg/z/
expat/ft2/icu/harfbuzz/dng/piex/log/base/cutils shared). See `Android.bp`.

## Wiring into a4l

a4l (`src/runtime/jvm_vm.cpp` `ArtVm::load_acl_graphics`) `dlopen`s the `.so` (path via
`ACL_ART_GRAPHICS_LIB`) and calls the exported `register_acl_graphics(JNIEnv*)` after the native
floor. Then real `android.graphics.Canvas`/`Bitmap`/`Paint` execute on host ART, rendered by
Android's Skia. Validated by `tests/fixtures/art_graphics/Main.java`.

**Next (S3b/S4):** back the `Canvas`'s `SkBitmap` with a4l's dma-buf-exported `VkImage`
(`GrBackendTexture` over the shared `VkDevice`) so app draws land in the presentable surface, then
present via `zwp_linux_dmabuf_v1` (already built, P2.4/V2b).

This is a minimal 2D-raster subset (drawColor/drawRect + Bitmap/Paint plumbing); text/paths/shaders
and the DisplayList/RenderThread design grow from here.

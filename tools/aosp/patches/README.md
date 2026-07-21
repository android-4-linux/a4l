<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: acl P2.4/S4a build tooling; patches external/skia (Apache-2.0/BSD). No GPL-3.0 sources. -->

# AOSP-tree patches for acl

Changes to the vendored AOSP tree (`$ACL_AOSP_ROOT`, default `/mnt/dev/aosp`, pinned
android-13.0.0_r84) that acl's host build needs. Kept here so the build is reproducible and the
modifications are reviewable (clean-room provenance). Apply into the AOSP checkout, then rebuild.

## `external-skia-host-vulkan.patch` (P2.4/S4a)

Enables the **Ganesh Vulkan** backend in Android's **host** `libskia` (`linux_glibc`). By default
the host Skia is CPU-only — `linux/include/config/SkUserConfig.h` sets `SK_SUPPORT_GPU 0`, and the
entire GPU/Vulkan source set + `SK_VULKAN`/`SK_USE_VMA` are gated to `target: { android }` in
`external/skia/Android.bp` (layoutlib never needed GPU). This patch:

- `linux/include/config/SkUserConfig.h`: `SK_SUPPORT_GPU 1` + define `SK_VULKAN` + `SK_USE_VMA`.
- `Android.bp`: add the GPU source set (~287 `src/gpu/*`, `src/image/*Gpu*`, `src/sksl/codegen/*`,
  the `GrVulkanMemoryAllocator`) to the `libskia` **`host_linux`** target block, minus the GL
  backend (`src/gpu/gl/*` — Vulkan-only host build), plus the vendored Vulkan headers on the include
  path. Same for `skia_arch_defaults`' VMA.

Skia calls Vulkan through consumer-provided function pointers (`GrVkGetProc` on `GrVkBackendContext`),
so `libskia` links **no** Vulkan loader — acl supplies the entrypoints. After applying:

```sh
cd $ACL_AOSP_ROOT && git -C external/skia apply <this patch>   # or patch -p1
lunch aosp_x86_64-eng
m out/soong/.intermediates/external/skia/libskia/linux_glibc_x86_64_static/libskia.a
# verify: nm -C .../libskia.a | grep 'GrDirectContext::MakeVulkan'   (578 GrVk* symbols)
```

**Why:** S4a GPU-native zero-copy — Android's Skia (the same Skia the `android.graphics` natives use)
renders `Canvas` draws straight into acl's dma-buf `VkImage` via Ganesh Vulkan, over a `VkDevice`
shared with acl's gfx, removing the S3b CPU upload. Only `libacl_graphics_jni` needs rebuilding
against the Vulkan-enabled `libskia`; other host tools keep their prebuilt CPU `libskia` unless
rebuilt. The CPU-render path (S3b) is unaffected.

## `chromium-webview-vulkan-functor.patch` (WebView hardware present)

Applies to the **Chromium** checkout (`/mnt/dev/chromium/src`, the `system_webview_apk` build), not
AOSP. Lets Chromium's WebView `AwDrawFn` **Vulkan functor** composite directly into acl's
Ganesh-Vulkan present frame (direct mode: viz + functor share acl's `VkDevice`, no AHardwareBuffer
round-trip). Four changes:

- `aw_draw_fn_impl.cc` — `CreateDrawContext` derives the `SkColorType` from `params->format` instead
  of hardcoded `MakeN32Premul` (acl's present target is **BGRA**, the Wayland/DRM-native byte order;
  N32 resolves to RGBA on this build, which fails `GrVkSecondaryCBDrawContext::Make`'s
  colortype↔format check). `DrawVk` gains `ScopedAllowBaseSyncPrimitives/BlockingForTesting` — acl
  calls the functor from its own present thread (its hwui-RenderThread equivalent), and the direct
  path blocks on the Viz thread (`ScheduleOnVizAndBlock`), which Android only permits on RenderThread.
- `aw_vulkan_context_provider.cc` — `ScopedAllowBlockingForTesting` around InitVk's libvulkan load.
- `gpu/vulkan/android/vulkan_implementation_android.cc` — the WSI surface extensions
  (`VK_KHR_surface`/`VK_KHR_android_surface`) become optional (required only when `using_surface`);
  acl renders offscreen and host Mesa exposes no Android WSI. Pair with `--disable-vulkan-surface`.
- `third_party/ashmem/ashmem-dev.c` — memfd-backed `ASharedMemory` (host has no `/dev/ashmem`).

Runtime (see `tools/run-apk.sh`): `ACL_WEBVIEW_VULKAN=1` auto-adds `--webview-draw-functor-uses-vulkan
--disable-vulkan-surface`. On a multi-ICD host, pin the loader to the GPU's ICD via
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json` (a non-matching ICD can crash
`vkCreateInstance`).

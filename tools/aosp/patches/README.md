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
round-trip). Seven changes across the render, GPU-raster, shared-image, and shared-memory paths:

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
- `gpu/command_buffer/service/shared_image/external_vk_image_backing_factory.cc` — `IsSupported`/
  `CreateSharedImage` allow **cross-thread** (`thread_safe`) images. On host Mesa the AHardwareBuffer
  factory is unavailable, so `ExternalVkImageBacking` is the *only* Vulkan backing — and WebView's
  GPU-raster tiles are cross-thread (rastered on a worker, read by the display compositor). The
  upstream rejection left tile content with no factory, so map tiles / page content never composited
  while solid backgrounds did. The backing already syncs GPU access with `VkSemaphore`s across
  Begin/EndAccess, covering the produce-then-consume tile pattern.
- `gpu/command_buffer/service/shared_image/external_vk_image_backing.cc` — `ProduceSkia` relaxes the
  `DCHECK_EQ(context_state_, context_state)`. With the display compositor on acl's separate
  RenderThread, raster and display use two `SharedContextState`s on the **same** acl `VkDevice`, so a
  tile backing created on the raster context is produced for the display context here — a mismatch the
  DCHECK never anticipated. The representation reads via the backing's own `context_state_` and the
  `VkImage` is device-level shareable, so it is safe. (Pairs with the factory change above; together
  they are what make tile pixels reach the composite.)
- `cc/trees/presentation_time_callback_buffer.cc` — bound the callback buffer instead of the
  `DCHECK_LE(size, kMaxBufferSize)` that fired **FATAL at 61 > 60** and killed the Compositor thread
  after ~60 frames. In the Vulkan `AwDrawFn` path presentation feedback is never delivered back
  (`AwGLSurface::SwapBuffers`, which arms it, is a GL-path mechanism the Vulkan output device
  bypasses), so `PopPendingCallbacks` never runs and the buffer grew unbounded. Drop the oldest stale
  entries; presentation-time callbacks don't drive rAF/rendering, so static content is unaffected.

Runtime (see `tools/run-apk.sh`): `ACL_WEBVIEW_VULKAN=1` auto-adds `--webview-draw-functor-uses-vulkan
--disable-vulkan-surface`. On a multi-ICD host, pin the loader to the GPU's ICD via
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json` (a non-matching ICD can crash
`vkCreateInstance`).

## `chromium-webview-present-feedback.patch` (WebView presentation timing)

Applies to the **Chromium** checkout, alongside `chromium-webview-vulkan-functor.patch`. Feeds acl's
real `wp_presentation` feedback into WebView's presentation feedback so its frame pacing /
presentation-time callbacks see true vsync timing instead of the draw-time `TimeTicks::Now()` + a
**zero interval** the Viz output device reports. One change:

- `components/viz/service/display_embedder/skia_output_device.cc` — `SwapInfo::CallFeedback` resolves
  `acl_webview_get_present_timing` from `libwebviewchromium_plat_support.so`
  (`dlopen(RTLD_NOLOAD)` + `dlsym` — `RTLD_DEFAULT` does not reach it in acl's bionic namespace) and,
  when it returns a non-zero present time, reports `PresentationFeedback(present_ns, refresh_ns,
  flags)` instead of the draw-time fallback.

acl side (present path): `dmabuf_presenter` exposes the last `wp_presentation` feedback via
`IPresenter::last_present_feedback` (CLOCK_MONOTONIC only — matches `base::TimeTicks`); `jvm_vm`
samples it on the present thread and, on the RenderThread before the functor draw, pushes it through
`webview_functor_vk` → `acl_libwebviewchromium_plat_support.so` (`acl_webview_functor_set_present_timing`
→ atomics read by the getter above). **Opt-in via `ACL_PRESENT_FEEDBACK`** — the gate is acl-side only
(Chromium's bionic `getenv` does not see acl-injected env vars; the getter returning 0 when the feature
is off is what keeps Chromium on its draw-time fallback). Confirmed end-to-end: Chromium reads acl's
real timestamp + a ~4.167 ms (240 Hz) interval.

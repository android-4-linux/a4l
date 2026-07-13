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

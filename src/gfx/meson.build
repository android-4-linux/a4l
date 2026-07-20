# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-4, Addendum A02. No GPL-3.0 sources.
#
# gfx/ — Skia-backed Canvas/Surface/Paint/Bitmap JNI (skia-shm/egl/vulkan-dmabuf
# via wcs canvases), text stack (Skia + HarfBuzz/ICU/fontconfig; minikin fallback).
# Single pinned Skia shared with wcs (no ODR duplication) — see A02.2. When a pinned
# Skia prefix is present (have_skia), the real raster backend (gfx_skia.cpp) is
# compiled in and linked against it; otherwise gfx is the self-contained software
# rasterizer only.
gfx_sources = [files('gfx.cpp')]
gfx_deps = []
if have_skia
  gfx_sources += files('gfx_skia.cpp')
  gfx_deps += skia_dep
endif
# The Ganesh Vulkan present target (P2.4/V1) — pinned Skia's Vulkan backend rendering
# into a GPU SkSurface (dma-buf export lands in V2). Needs system vulkan + libdrm.
if have_vulkan
  gfx_sources += files('gfx_vk.cpp')
  gfx_deps += [vulkan_dep, libdrm_dep]
endif
# The host GL device (GLSurfaceView Stage 1): gbm-backed headless GLES rendering into a
# dma-buf, presented via the same zwp_linux_dmabuf_v1 path as the Vulkan frame. Needs
# EGL + GLESv2 + gbm + libdrm (drm_fourcc.h). gl_egl.h includes gfx_vk.h for DmabufFrame,
# so this is compiled independently of have_vulkan.
if have_gl
  gfx_sources += files('gl_egl.cpp')
  # HardwareBuffer: a gbm_bo exported as a dma-buf (gfx::DmabufFrame) — the zero-copy graphics
  # buffer under Flutter's external-texture path and android.hardware.HardwareBuffer.
  gfx_sources += files('hardware_buffer.cpp')
  # AHardwareBuffer representation + the EGL_NATIVE_BUFFER_ANDROID -> EGL_LINUX_DMA_BUF_EXT
  # translation the engine's texture import needs (M2).
  gfx_sources += files('ahardware_buffer.cpp')
  gfx_deps += [egl_dep, glesv2_dep, gbm_dep, gl_libdrm_dep, wl_egl_dep]
  # GLSurfaceView Stage 2a: the com.google.android.gles_jni.EGLImpl natives (EGL10 on host
  # libEGL). Needs JNI headers; compiled only when the JVM/JNI is present.
  if have_jni
    gfx_sources += files('egl10_jni.cpp')
    gfx_deps += jni_dep
  endif
endif
gfx_lib = static_library(
  'acl_gfx',
  gfx_sources,
  include_directories : [config_inc, core_inc],
  dependencies : gfx_deps,
)
# Propagate Skia's link args to whoever links acl_gfx (the archives resolve at final
# link, not in the static lib).
gfx_dep = declare_dependency(
  link_with : gfx_lib,
  dependencies : gfx_deps,
)
module_libs += gfx_lib

# Dev tool + M1 proof for the Flutter external-texture bridge: allocate a HardwareBuffer, CPU-fill a
# pattern, import its dma-buf as an EGLImage, sample it via GL_TEXTURE_EXTERNAL_OES, and read back
# bit-exact. Headless (surfaceless EGL on a render node). Built only when the GL backend is present.
if have_gl
  executable(
    'acl-hwbuf-probe',
    files('hwbuf_probe_main.cpp'),
    include_directories : [config_inc, core_inc],
    link_with : [gfx_lib, acl_log],
    dependencies : [fmt_dep, egl_dep, glesv2_dep, gbm_dep, gl_libdrm_dep],
    install : false,
  )
endif

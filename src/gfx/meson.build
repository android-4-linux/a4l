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

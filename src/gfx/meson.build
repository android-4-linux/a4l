# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-4, Addendum A02. No GPL-3.0 sources.
#
# gfx/ — Skia-backed Canvas/Surface/Paint/Bitmap JNI (skia-shm/egl/vulkan-dmabuf
# via wcs canvases), text stack (Skia + HarfBuzz/ICU/fontconfig; minikin fallback).
# Single pinned Skia shared with wcs (no ODR duplication) — see A02.2.
module_libs += static_library(
  'aclcxx_gfx',
  files('gfx.cpp'),
  include_directories : [config_inc, core_inc],
)

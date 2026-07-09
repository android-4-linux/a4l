# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-4, ADR-6, Addendum A02, §4 Phase 2. No GPL-3.0 sources.
#
# wm/ — Window/Activity surface management over wcs (xdg-shell, wl_subsurface
# for SurfaceView, wp_viewport + wp_fractional_scale_v1, xdg-activation,
# wp_presentation). ViewRootImpl-equivalent, Choreographer, damage.
module_libs += static_library(
  'aclcxx_wm',
  files('wm.cpp'),
  include_directories : config_inc,
)

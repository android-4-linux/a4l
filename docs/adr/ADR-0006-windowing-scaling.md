<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0006: Windowing/scaling — wcs + fractional-scale + viewport

- **Status:** Accepted
- **Date:** 2026-07-09
- **Provenance:** plan.md §3 ADR-6, Addendum A02.1, R-9. No GPL-3.0 sources.

## Context
The layer must present as a native Wayland client across compositors (Mutter, Sway, wlroots,
agl) with correct HiDPI/physical-pixel behavior, and map SurfaceView correctly.

## Decision
All surfaces go through **wcs** CRTP bindings: xdg-shell toplevels/popups, `wl_subsurface`
(SurfaceView), `xdg-activation-v1`, `wp_presentation`. Scaling uses **`wp_fractional_scale_v1`
+ `wp_viewport`** as primary, with integer `wl_output.scale` fallback — identical policy to the
ivi-homescreen physical-pixel patch; implement as shared code where practical.

## Consequences
Seat/cursor/data-device reuse the landed wcs `SeatManager`, `CursorManager`, `DataDevice`. IME/
clipboard/activation gaps on some compositors (R-9) are handled by the same portability matrix +
fallbacks as the flatpak-activation plugin spec. Known wcs gaps on the critical path:
dmabuf-feedback-v4 and syncobj bindings (scheduled as Phase 2 wcs upstream work).

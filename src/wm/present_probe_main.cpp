// SPDX-License-Identifier: Apache-2.0
// Provenance: original test harness; plan.md §5 (Tier C compositor matrix),
// Addendum A02. No GPL-3.0 sources.
//
// Tier-C presentation probe: raster a gfx::Surface (blue top half, red bottom
// half — the same content the View traversal produces) and present it to a
// Wayland compositor via wm/present (wcs shm path). run_present_test.py runs this
// against a headless Weston. Exit 0 iff the frame was shown (frame callback).
#include <cstdio>
#include <cstdint>

#include "gfx/gfx.h"
#include "wm/present.h"

int main() {
  constexpr int kW = 256, kH = 256;
  aclcxx::gfx::Surface surface(kW, kH);
  surface.fill_rect(0, 0, kW, kH / 2, 0xFF0000FFu);      // blue top
  surface.fill_rect(0, kH / 2, kW, kH, 0xFFFF0000u);     // red bottom

  if (!aclcxx::wm::wayland_available()) {
    std::fprintf(stderr, "wm-present-probe: no Wayland compositor reachable\n");
    return 2;
  }
  const bool ok = aclcxx::wm::present_surface(surface, "aclcxx present probe",
                                              "com.aclcxx.present.probe", 5000);
  std::fprintf(stderr, "wm-present-probe: %s\n", ok ? "PRESENTED-OK" : "PRESENT-FAILED");
  return ok ? 0 : 1;
}

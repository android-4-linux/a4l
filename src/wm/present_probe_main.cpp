// SPDX-License-Identifier: Apache-2.0
// Provenance: original test harness; plan.md §5 (Tier C compositor matrix),
// Addendum A02. No GPL-3.0 sources.
//
// Tier-C presentation probe. Two modes:
//   (default)      one frame: raster a blue/red gfx::Surface and present it
//   once.
//   --frames <N>   N frames: run the Choreographer-style present_loop,
//   animating a
//                  moving band so every frame differs; each wl_surface.frame
//                  tick redraws with a wp_presentation-aligned frame time.
// run_present_test.py runs this against a headless Weston.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gfx/gfx.h"
#include "wm/present.h"

namespace {
constexpr int kW = 256, kH = 256;

int present_once() {
  acl::gfx::Surface surface(kW, kH);
  surface.fill_rect(0, 0, kW, kH / 2, 0xFF0000FFu);   // blue top
  surface.fill_rect(0, kH / 2, kW, kH, 0xFFFF0000u);  // red bottom
  const bool ok = acl::wm::present_surface(surface, "acl present probe",
                                           "com.acl.present.probe", 5000);
  std::fprintf(stderr, "wm-present-probe: %s\n",
               ok ? "PRESENTED-OK" : "PRESENT-FAILED");
  return ok ? 0 : 1;
}

int present_animated(std::uint64_t frames) {
  // A red band sweeping down a blue field — each frame is distinct, so the loop
  // genuinely produces `frames` different frames rather than re-showing one
  // buffer.
  const acl::wm::FrameCallback render = [&](acl::gfx::Surface& s,
                                            std::uint64_t /*frame_time_ns*/,
                                            std::uint64_t i) {
    s.draw_color(0xFF0000FFu);  // blue field
    const int band = 24;
    const int y = static_cast<int>((i * 16) % static_cast<std::uint64_t>(kH));
    s.fill_rect(0, y, kW, y + band, 0xFFFF0000u);  // moving red band
    return true;  // keep going; the loop stops at max_frames
  };
  const bool ok =
      acl::wm::present_loop(kW, kH, "acl present loop", "com.acl.present.loop",
                            render, acl::wm::InputCallback{}, false, frames);
  std::fprintf(stderr, "wm-present-probe: %s\n",
               ok ? "LOOP-PRESENTED-OK" : "LOOP-FAILED");
  return ok ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  if (!acl::wm::wayland_available()) {
    std::fprintf(stderr, "wm-present-probe: no Wayland compositor reachable\n");
    return 2;
  }
  if (argc >= 3 && std::strcmp(argv[1], "--frames") == 0) {
    return present_animated(static_cast<std::uint64_t>(std::atoll(argv[2])));
  }
  return present_once();
}

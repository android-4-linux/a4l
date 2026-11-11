// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §2, ADR-6, Addendum A02. No GPL-3.0 sources.
//
// Fallback wm/ presenter, compiled when wcs + wayland-client are NOT available
// (ACL_HAVE_WCS off). Presentation reports as unavailable; the rest of acl
// (View traversal, gfx raster, headless pixel verification) is unaffected.

#include "wm/present.h"

#include "log/log.h"

namespace acl::wm {

constexpr const char* kMsg =
    "Wayland presentation unavailable: built without wcs/wayland-client "
    "(-Dwcs_root=<path>); the frame was rendered but not shown";

bool wayland_available() noexcept {
  return false;
}

bool query_display(int&, int&, int&) noexcept {
  return false;
}

bool present_surface(const gfx::Surface&,
                     std::string_view,
                     std::string_view,
                     int) noexcept {
  log::w("acl.wm", kMsg);
  return false;
}

bool present_loop(int,
                  int,
                  std::string_view,
                  std::string_view,
                  const FrameCallback&,
                  const InputCallback&,
                  bool,
                  std::uint64_t) noexcept {
  log::w("acl.wm", kMsg);
  return false;
}

}  // namespace acl::wm

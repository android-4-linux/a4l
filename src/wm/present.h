// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §2, ADR-6, Addendum A02 (windowing via wayland-cxx-scanner).
// Android SDK ViewRootImpl/Surface docs (behavioral). No GPL-3.0 sources.
#ifndef ACLCXX_WM_PRESENT_H
#define ACLCXX_WM_PRESENT_H

// The windowing seam: present an aclcxx::gfx::Surface (a software ARGB_8888 frame
// produced by the View traversal) to a Wayland compositor. This is the narrow,
// wcs-free boundary the rest of aclcxx sees; the wcs/CRTP/xdg-shell machinery lives
// entirely in wayland_presenter.cpp and is compiled only when wcs + wayland-client
// are available (ACLCXX_HAVE_WCS). Without them, present_surface() reports the gap
// via the stub. Continuous presentation (Choreographer on wp_presentation, damage,
// multi-buffering) builds on this one-frame primitive.

#include <string_view>

#include "gfx/gfx.h"

namespace aclcxx::wm {

// True if a Wayland presentation backend is compiled in AND a compositor is
// reachable (WAYLAND_DISPLAY connects). Cheap; used to decide whether to present.
bool wayland_available() noexcept;

// Present one frame: connect to the compositor, create an xdg_toplevel titled
// `title`/`app_id`, upload `surface`'s pixels into a wl_shm buffer, commit, and
// block until the compositor confirms the frame (wl_surface.frame callback) or
// `timeout_ms` elapses. Returns true when the frame was shown. A one-shot present
// (the surface is torn down on return) — enough to prove the seam and to snapshot.
bool present_surface(const gfx::Surface& surface, std::string_view title,
                     std::string_view app_id, int timeout_ms = 3000) noexcept;

const char* module_name() noexcept;

}  // namespace aclcxx::wm

#endif  // ACLCXX_WM_PRESENT_H

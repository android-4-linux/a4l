// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l code. The ivi-shell client role follows the public
// ivi-application protocol (third_party/wayland-protocols/ivi-application.xml,
// MIT). No GPL-3.0 sources.
//
// Which shell a presenter asks for its window role, read from the environment.
// `a4l run --ivi-surface-id=<n>` sets both variables.
#ifndef ACL_WM_SHELL_ENV_H
#define ACL_WM_SHELL_ENV_H

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace acl::wm {

// ACL_WL_SHELL=ivi: the window is an ivi_surface (ivi-shell), not an
// xdg_toplevel.
inline bool ivi_shell_selected() noexcept {
  const char* s = std::getenv("ACL_WL_SHELL");
  return s != nullptr && std::string_view(s) == "ivi";
}

// ACL_IVI_SURFACE_ID: the compositor-wide id the surface is created under.
// ivi-shell has no way to hand one out, so 0 means "not given".
inline std::uint32_t ivi_surface_id_from_env() noexcept {
  const char* s = std::getenv("ACL_IVI_SURFACE_ID");
  if (s == nullptr || *s == '\0')
    return 0;
  return static_cast<std::uint32_t>(std::strtoul(s, nullptr, 0));
}

}  // namespace acl::wm

#endif  // ACL_WM_SHELL_ENV_H

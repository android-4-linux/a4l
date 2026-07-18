// SPDX-License-Identifier: Apache-2.0
// Provenance: XDG Desktop Portal Settings spec (org.freedesktop.portal.Settings.Read,
// namespace org.freedesktop.appearance, key color-scheme). sdbus-cpp usage after
// jwinarske/sdbus-cpp-examples (Apache-2.0) and svc/battery. No GPL-3.0 sources.
#include "svc/appearance/appearance.h"

#include "acl_config.h"
#include "log/log.h"

#include <cstdint>
#include <string>
#include <string_view>

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::appearance {
namespace {
constexpr std::string_view kTag = "svc.appearance";
}  // namespace

#if ACL_HAVE_SDBUS

int query_color_scheme() noexcept {
  try {
    auto conn = sdbus::createSessionBusConnection();  // the portal lives on the session bus
    auto portal = sdbus::createProxy(
        *conn, sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
        sdbus::ObjectPath{"/org/freedesktop/portal/desktop"});
    sdbus::Variant value;
    portal->callMethod("Read")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.portal.Settings"})
        .withArguments(std::string{"org.freedesktop.appearance"}, std::string{"color-scheme"})
        .storeResultsTo(value);
    // Read returns (v); the color-scheme value is a u, but some portal versions double-wrap it
    // in another variant — accept either shape.
    std::uint32_t scheme = 0;
    try {
      scheme = value.get<std::uint32_t>();
    } catch (const sdbus::Error&) {
      scheme = value.get<sdbus::Variant>().get<std::uint32_t>();
    }
    log::i(kTag, "color-scheme = {} (0=none 1=dark 2=light)", scheme);
    return static_cast<int>(scheme);
  } catch (const sdbus::Error& e) {
    log::i(kTag, "color-scheme unavailable ({}); keeping default", e.getName());
    return -1;
  } catch (...) {
    return -1;
  }
}

#else

int query_color_scheme() noexcept { return -1; }

#endif

}  // namespace acl::appearance

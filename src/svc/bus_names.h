// SPDX-License-Identifier: Apache-2.0
// Provenance: the D-Bus specification's bus driver interface
// (org.freedesktop.DBus: NameHasOwner, ListActivatableNames,
// StartServiceByName); sdbus-c++ usage after jwinarske/sdbus-cpp-examples
// (Apache-2.0). No GPL-3.0 sources.
#ifndef ACL_SVC_BUS_NAMES_H
#define ACL_SVC_BUS_NAMES_H
// Whether a service a backend wants is on a bus. A desktop session has most of
// them running. A system with no desktop has them installed and not started:
// the bus starts UPower or UDisks2 when something first calls it. A backend
// that asks only whether the name has an owner finds nothing there.
//
// Header-only and only for code built with sdbus-c++ (ACL_HAVE_SDBUS).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

namespace acl::svc {

// How long the bus is given to start a service. A unit that fails to start
// would otherwise hold the caller for the default 25 s.
inline constexpr std::chrono::seconds kBusStartTimeout{5};

// True when `bus_name` has an owner, or the bus lists it as one it can start
// and does start it within kBusStartTimeout. False otherwise, and on any
// error.
inline bool bus_name_available(sdbus::IConnection& conn, const char* bus_name) {
  try {
    const sdbus::InterfaceName iface{"org.freedesktop.DBus"};
    auto dbus =
        sdbus::createProxy(conn, sdbus::ServiceName{"org.freedesktop.DBus"},
                           sdbus::ObjectPath{"/org/freedesktop/DBus"});
    const std::string name{bus_name};
    bool owned = false;
    dbus->callMethod("NameHasOwner")
        .onInterface(iface)
        .withArguments(name)
        .storeResultsTo(owned);
    if (owned)
      return true;
    std::vector<std::string> startable;
    dbus->callMethod("ListActivatableNames")
        .onInterface(iface)
        .storeResultsTo(startable);
    if (std::find(startable.begin(), startable.end(), name) == startable.end())
      return false;
    std::uint32_t result = 0;  // 1 started, 2 was running already
    dbus->callMethod("StartServiceByName")
        .onInterface(iface)
        .withTimeout(kBusStartTimeout)
        .withArguments(name, std::uint32_t{0})
        .storeResultsTo(result);
    return result == 1 || result == 2;
  } catch (const sdbus::Error&) {
    return false;
  }
}

}  // namespace acl::svc

#endif  // ACL_SVC_BUS_NAMES_H

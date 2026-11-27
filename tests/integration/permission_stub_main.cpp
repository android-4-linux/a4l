// SPDX-License-Identifier: Apache-2.0
// Provenance: original test helper; the XDG permission store D-Bus interface
// (org.freedesktop.impl.portal.PermissionStore, xdg-desktop-portal
// documentation); sdbus-c++ usage after jwinarske/sdbus-cpp-examples
// (Apache-2.0). No GPL-3.0 sources.
//
// A stand-in permission store for run_permission_test.py. It owns
// org.freedesktop.impl.portal.PermissionStore on the session bus it is started
// on and answers GetPermission for the devices table: "yes" once the file named
// on its command line exists, no entry before that. It prints each call.
#include <sdbus-c++/sdbus-c++.h>

#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr const char* kService = "org.freedesktop.impl.portal.PermissionStore";
constexpr const char* kPath = "/org/freedesktop/impl/portal/PermissionStore";
constexpr const char* kIface = "org.freedesktop.impl.portal.PermissionStore";

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: acl-permission-stub <grant file>\n");
    return 2;
  }
  const std::string grant = argv[1];
  auto conn = sdbus::createSessionBusConnection(sdbus::ServiceName{kService});
  auto object = sdbus::createObject(*conn, sdbus::ObjectPath{kPath});

  auto get = [&grant](const std::string& table, const std::string& id,
                      const std::string& app) -> std::vector<std::string> {
    if (table != "devices") {
      throw sdbus::Error(
          sdbus::Error::Name{"org.freedesktop.portal.Error.NotFound"},
          "No entry for " + table);
    }
    const bool yes = ::access(grant.c_str(), F_OK) == 0;
    std::printf("STUB: get %s %s %s -> %s\n", table.c_str(), id.c_str(),
                app.c_str(), yes ? "yes" : "unset");
    std::fflush(stdout);
    return yes ? std::vector<std::string>{"yes"} : std::vector<std::string>{};
  };

  object->addVTable(sdbus::registerMethod("GetPermission").implementedAs(get))
      .forInterface(sdbus::InterfaceName{kIface});

  std::printf("STUB: ready\n");
  std::fflush(stdout);
  conn->enterEventLoop();
  return 0;
}

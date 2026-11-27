// SPDX-License-Identifier: Apache-2.0
// Provenance: original test helper; the UPower D-Bus interface
// (org.freedesktop.UPower, org.freedesktop.UPower.Device, upower
// documentation); sdbus-c++ usage after jwinarske/sdbus-cpp-examples
// (Apache-2.0). No GPL-3.0 sources.
//
// A stand-in UPower for run_bus_activation_test.py. It owns
// org.freedesktop.UPower on the bus named by DBUS_SYSTEM_BUS_ADDRESS and has
// one display device: a battery at 73 %, discharging. With ACL_STUB_NO_BATTERY
// set the device is what UPower shows on a machine with no battery: not
// present, 0 %, state unknown.
#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main() {
  const sdbus::ObjectPath root{"/org/freedesktop/UPower"};
  const sdbus::ObjectPath device{
      "/org/freedesktop/UPower/devices/DisplayDevice"};
  const sdbus::InterfaceName dev_iface{"org.freedesktop.UPower.Device"};

  const bool battery = std::getenv("ACL_STUB_NO_BATTERY") == nullptr;

  auto conn = sdbus::createSystemBusConnection(
      sdbus::ServiceName{"org.freedesktop.UPower"});
  auto up = sdbus::createObject(*conn, root);
  up->addVTable(sdbus::registerMethod("GetDisplayDevice").implementedAs([] {
      return sdbus::ObjectPath{"/org/freedesktop/UPower/devices/DisplayDevice"};
    }))
      .forInterface(sdbus::InterfaceName{"org.freedesktop.UPower"});

  auto dev = sdbus::createObject(*conn, device);
  dev->addVTable(
         sdbus::registerProperty("Percentage").withGetter([battery] {
           return battery ? 73.0 : 0.0;
         }),
         sdbus::registerProperty("Temperature").withGetter([] { return 30.0; }),
         sdbus::registerProperty("Voltage").withGetter([] { return 3.9; }),
         sdbus::registerProperty("State").withGetter([battery] {
           return std::uint32_t{battery ? 2U : 0U};  // discharging, unknown
         }),
         sdbus::registerProperty("Technology").withGetter([] {
           return std::uint32_t{1};  // lithium ion
         }),
         sdbus::registerProperty("IsPresent").withGetter([battery] {
           return battery;
         }))
      .forInterface(dev_iface);

  std::printf("STUB: ready\n");
  std::fflush(stdout);
  conn->enterEventLoop();
  return 0;
}

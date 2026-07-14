// SPDX-License-Identifier: Apache-2.0
// Provenance: UPower org.freedesktop.UPower / org.freedesktop.UPower.Device D-Bus
// API docs → android.os.BatteryManager / ACTION_BATTERY_CHANGED extras (AOSP
// android.jar stubs, Apache-2.0). sdbus-cpp usage after jwinarske/sdbus-cpp-examples
// (Apache-2.0). No GPL-3.0 sources.
#include "svc/battery/battery_backend.h"

#include "acl_config.h"
#include "log/log.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::battery {
namespace {

constexpr std::string_view kTag = "svc.battery";

// The Null backend: no daemon (or no sdbus). Static healthy defaults so a battery
// read never NPEs — a desktop with no battery still reports a present, full cell.
class NullBackend : public IBatteryBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  AndroidBatteryState snapshot() const override { return {}; }
  void set_sink(std::function<void(const AndroidBatteryState&)>) override {}
};

#if ACL_HAVE_SDBUS

// BatteryManager status/technology constants (android.os.BatteryManager).
constexpr int kStatusUnknown = 1, kStatusCharging = 2, kStatusDischarging = 3,
              kStatusNotCharging = 4, kStatusFull = 5;
constexpr int kPluggedNone = 0, kPluggedAc = 1;

// UPower Device.State (u) → BatteryManager EXTRA_STATUS.
int map_state(std::uint32_t upower_state) {
  switch (upower_state) {
    case 1: return kStatusCharging;      // charging
    case 2: return kStatusDischarging;   // discharging
    case 3: return kStatusDischarging;   // empty
    case 4: return kStatusFull;          // fully-charged
    case 5:                              // pending-charge
    case 6: return kStatusNotCharging;   // pending-discharge
    default: return kStatusUnknown;
  }
}
// UPower Device.Technology (u) → EXTRA_TECHNOLOGY string (Android's convention).
std::string map_technology(std::uint32_t tech) {
  switch (tech) {
    case 1: return "Li-ion";
    case 2: return "Li-poly";
    case 3: return "LiFe";       // lithium-iron-phosphate
    case 4: return "Lead-acid";
    case 5: return "NiCd";
    case 6: return "NiMH";
    default: return "Li-ion";
  }
}

bool name_has_owner(sdbus::IConnection& conn, const char* bus_name) {
  try {
    auto dbus = sdbus::createProxy(conn, sdbus::ServiceName{"org.freedesktop.DBus"},
                                   sdbus::ObjectPath{"/org/freedesktop/DBus"});
    bool owned = false;
    dbus->callMethod("NameHasOwner")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus"})
        .withArguments(std::string{bus_name})
        .storeResultsTo(owned);
    return owned;
  } catch (const sdbus::Error&) {
    return false;
  }
}

// UPower backend: read the composite DisplayDevice (GetDisplayDevice) — a virtual
// device that aggregates the system's power supplies, always present — and map its
// org.freedesktop.UPower.Device properties to AndroidBatteryState. Property changes
// on the device re-fire the sink.
class UPowerBackend : public IBatteryBackend {
 public:
  explicit UPowerBackend(std::unique_ptr<sdbus::IConnection> conn) : conn_(std::move(conn)) {
    sdbus::ObjectPath display{"/org/freedesktop/UPower/devices/DisplayDevice"};
    try {
      auto up = sdbus::createProxy(*conn_, sdbus::ServiceName{"org.freedesktop.UPower"},
                                   sdbus::ObjectPath{"/org/freedesktop/UPower"});
      up->callMethod("GetDisplayDevice")
          .onInterface(sdbus::InterfaceName{"org.freedesktop.UPower"})
          .storeResultsTo(display);
    } catch (const sdbus::Error& e) {
      log::w(kTag, "UPower GetDisplayDevice failed ({}); using default DisplayDevice path",
             e.getMessage());
    }
    device_path_ = display;
    dev_ = sdbus::createProxy(*conn_, sdbus::ServiceName{"org.freedesktop.UPower"}, device_path_);
    // Re-read + notify on any device property change.
    dev_->uponSignal("PropertiesChanged")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.Properties"})
        .call([this](const std::string&, const std::map<std::string, sdbus::Variant>&,
                     const std::vector<std::string>&) { notify(); });
    conn_->enterEventLoopAsync();
  }
  ~UPowerBackend() override {
    try { conn_->leaveEventLoop(); } catch (...) {}
  }

  const char* name() const noexcept override { return "upower"; }

  AndroidBatteryState snapshot() const override {
    AndroidBatteryState s;  // defaults if a read throws
    const sdbus::InterfaceName dev{"org.freedesktop.UPower.Device"};
    try {
      const double pct = dev_->getProperty("Percentage").onInterface(dev).get<double>();
      const double temp = dev_->getProperty("Temperature").onInterface(dev).get<double>();
      const double volt = dev_->getProperty("Voltage").onInterface(dev).get<double>();
      const std::uint32_t state = dev_->getProperty("State").onInterface(dev).get<std::uint32_t>();
      const std::uint32_t tech = dev_->getProperty("Technology").onInterface(dev).get<std::uint32_t>();
      const bool present = dev_->getProperty("IsPresent").onInterface(dev).get<bool>();

      s.level = static_cast<int>(std::lround(pct));
      s.scale = 100;
      s.temperature_dc = static_cast<int>(std::lround(temp * 10.0));
      s.voltage_mv = static_cast<int>(std::lround(volt * 1000.0));
      s.status = map_state(state);
      s.plugged = (state == 1 || state == 4) ? kPluggedAc : kPluggedNone;  // charging/full ⇒ AC
      s.present = present;
      s.technology = map_technology(tech);
    } catch (const sdbus::Error& e) {
      log::w(kTag, "UPower device read failed ({}); battery defaults", e.getMessage());
    }
    return s;
  }

  void set_sink(std::function<void(const AndroidBatteryState&)> on_change) override {
    std::lock_guard<std::mutex> lk(sink_mu_);
    sink_ = std::move(on_change);
  }

 private:
  void notify() {
    std::function<void(const AndroidBatteryState&)> sink;
    { std::lock_guard<std::mutex> lk(sink_mu_); sink = sink_; }
    if (sink) sink(snapshot());
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  sdbus::ObjectPath device_path_{"/org/freedesktop/UPower/devices/DisplayDevice"};
  std::unique_ptr<sdbus::IProxy> dev_;
  std::mutex sink_mu_;
  std::function<void(const AndroidBatteryState&)> sink_;
};

#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<IBatteryBackend> make_battery_backend() {
#if ACL_HAVE_SDBUS
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSystemBusConnection();
  } catch (const sdbus::Error& e) {
    log::w(kTag, "no system bus ({}); battery defaults (null backend)", e.getMessage());
    return std::make_unique<NullBackend>();
  }
  const char* env = std::getenv("ACL_BATTERY_BACKEND");
  const std::string_view pref = env != nullptr ? env : "";
  if ((pref.empty() || pref == "upower") && name_has_owner(*conn, "org.freedesktop.UPower")) {
    log::i(kTag, "battery backend: upower");
    return std::make_unique<UPowerBackend>(std::move(conn));
  }
  log::i(kTag, "no UPower on the bus; battery defaults (null backend)");
#endif  // ACL_HAVE_SDBUS
  return std::make_unique<NullBackend>();
}

}  // namespace acl::battery

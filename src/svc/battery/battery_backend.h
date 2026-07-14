// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.os.BatteryManager / ACTION_BATTERY_CHANGED public
// surface (AOSP android.jar stubs, Apache-2.0); host mapping from freedesktop
// UPower (org.freedesktop.UPower / .Device) D-Bus API docs; sdbus-cpp usage
// pattern after jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
//
// svc/battery translates the host power daemon (UPower) into the slice of battery
// state Android apps read via the sticky ACTION_BATTERY_CHANGED broadcast /
// BatteryManager. The backend reduces UPower's DisplayDevice to one daemon-agnostic
// AndroidBatteryState; the Java BatteryBridge + the JNI seam speak only that.
#ifndef ACL_SVC_BATTERY_BATTERY_BACKEND_H
#define ACL_SVC_BATTERY_BATTERY_BACKEND_H

#include <functional>
#include <memory>
#include <string>

namespace acl::battery {

// The Android-visible battery truth (the ACTION_BATTERY_CHANGED extras), independent
// of the host daemon. Fields use Android's units/encodings so the Java bridge is a
// straight putExtra. Defaults describe a healthy, present, discharging battery.
struct AndroidBatteryState {
  int level = 100;             // EXTRA_LEVEL (current charge, 0..scale)
  int scale = 100;             // EXTRA_SCALE
  int temperature_dc = 0;      // EXTRA_TEMPERATURE (tenths of a degree C; 0 = no sensor)
  int voltage_mv = 0;          // EXTRA_VOLTAGE (millivolts)
  int status = 3;              // EXTRA_STATUS (BatteryManager: 1 unk,2 chg,3 dischg,4 not-chg,5 full)
  int health = 2;              // EXTRA_HEALTH (2 = GOOD)
  int plugged = 0;             // EXTRA_PLUGGED (0 none,1 AC,2 USB,4 wireless)
  bool present = true;         // EXTRA_PRESENT
  std::string technology = "Li-ion";  // EXTRA_TECHNOLOGY

  bool operator==(const AndroidBatteryState&) const noexcept = default;
};

// A backend owns a D-Bus proxy to the host power daemon. snapshot() answers the
// synchronous read (registerReceiver's returned sticky Intent); set_sink() arms an
// async callback fired on every UPower property change (→ re-broadcast in L1).
class IBatteryBackend {
 public:
  virtual ~IBatteryBackend() = default;

  // Human-readable backend id for logs ("upower", "null").
  virtual const char* name() const noexcept = 0;

  // Current best-effort battery snapshot.
  virtual AndroidBatteryState snapshot() const = 0;

  // Register the change sink (at most one; a second call replaces it). May be a no-op
  // for backends without change signals.
  virtual void set_sink(std::function<void(const AndroidBatteryState&)> on_change) = 0;
};

// Probe the system bus and construct the backend that owns it: UPower when present
// (ACL_BATTERY_BACKEND env override honored), else the Null backend (static healthy
// defaults). Never returns null.
std::unique_ptr<IBatteryBackend> make_battery_backend();

}  // namespace acl::battery

#endif  // ACL_SVC_BATTERY_BATTERY_BACKEND_H

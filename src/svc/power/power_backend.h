// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.os.PowerManager / IThermalService public surface (AOSP
// android.jar stubs, Apache-2.0); host mapping from systemd-logind
// (org.freedesktop.login1 Manager/Session) and power-profiles
// (org.freedesktop.UPower.PowerProfiles) D-Bus API docs, plus the kernel thermal
// sysfs ABI (Documentation/driver-api/thermal, GPL-doc'd ABI — interface only, no
// code). sdbus-cpp usage pattern after jwinarske/sdbus-cpp-examples (Apache-2.0).
// No GPL-3.0 sources.
//
// svc/power translates the host session/power daemons into the slice of power state
// Android apps read via android.os.PowerManager: whether the display is usable
// (isInteractive), whether the system is in a battery-saver profile
// (isPowerSaveMode), whether the session has gone idle (isDeviceIdleMode), and the
// current thermal throttling level (IThermalService). Wake locks are real: a held
// Android wake lock maps to a logind inhibitor fd, so acquiring one genuinely keeps
// the host from idling/suspending, and releasing it lets go.
#ifndef ACL_SVC_POWER_POWER_BACKEND_H
#define ACL_SVC_POWER_POWER_BACKEND_H

#include <functional>
#include <memory>
#include <string>

namespace acl::power {

// PowerManager.THERMAL_STATUS_* (android.os.PowerManager).
enum ThermalStatus : int {
  kThermalNone = 0,
  kThermalLight = 1,
  kThermalModerate = 2,
  kThermalSevere = 3,
  kThermalCritical = 4,
  kThermalEmergency = 5,
  kThermalShutdown = 6,
};

// PowerManager wake-lock level bits acl distinguishes (android.os.PowerManager).
// PARTIAL keeps the CPU alive only; the SCREEN_*/FULL levels also keep the display on.
constexpr int kPartialWakeLock = 0x00000001;
constexpr int kScreenDimWakeLock = 0x00000006;
constexpr int kScreenBrightWakeLock = 0x0000000a;
constexpr int kFullWakeLock = 0x0000001a;

// The Android-visible power truth, independent of the host daemons. Defaults describe
// an awake, unthrottled desktop session (what a machine with no logind/profiles is).
struct AndroidPowerState {
  bool interactive = true;   // isInteractive(): session active, not locked, not idle
  bool power_save = false;   // isPowerSaveMode(): host profile == power-saver
  bool device_idle = false;  // isDeviceIdleMode(): session IdleHint
  int thermal_status = kThermalNone;  // IThermalService.getCurrentThermalStatus()

  bool operator==(const AndroidPowerState&) const noexcept = default;
};

// A backend owns proxies to the host session/power daemons. snapshot() answers the
// synchronous reads; set_sink() arms an async callback fired when the session or the
// power profile changes. acquire_wake_lock() takes a real host inhibitor.
class IPowerBackend {
 public:
  virtual ~IPowerBackend() = default;

  // Human-readable backend id for logs ("logind", "null").
  virtual const char* name() const noexcept = 0;

  // Current best-effort power snapshot.
  virtual AndroidPowerState snapshot() const = 0;

  // Register the change sink (at most one; a second call replaces it). May be a no-op
  // for backends without change signals.
  virtual void set_sink(std::function<void(const AndroidPowerState&)> on_change) = 0;

  // Take a host inhibitor for an Android wake lock. `flags` is the PowerManager level
  // (kPartialWakeLock etc.); a screen-level lock also blocks idle/screen-blank.
  // Returns an opaque non-zero cookie, or 0 when no inhibitor could be taken (the
  // caller still treats the lock as held — Android semantics never fail acquire()).
  virtual long acquire_wake_lock(int flags, const std::string& tag) = 0;

  // Drop the inhibitor taken for `cookie` (no-op for 0 / unknown cookies).
  virtual void release_wake_lock(long cookie) = 0;
};

// Probe the system bus and construct the backend that owns it: logind when present
// (ACL_POWER_BACKEND env override honored), else the Null backend (awake defaults,
// wake locks accepted and dropped). Never returns null.
std::unique_ptr<IPowerBackend> make_power_backend();

}  // namespace acl::power

#endif  // ACL_SVC_POWER_POWER_BACKEND_H

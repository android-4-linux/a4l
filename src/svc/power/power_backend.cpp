// SPDX-License-Identifier: Apache-2.0
// Provenance: systemd-logind (org.freedesktop.login1 Manager/Session: Inhibit,
// GetSessionByPID, Active/IdleHint/LockedHint) and power-profiles-daemon
// (org.freedesktop.UPower.PowerProfiles / net.hadess.PowerProfiles ActiveProfile)
// D-Bus API docs → android.os.PowerManager / IThermalService (AOSP android.jar
// stubs, Apache-2.0); kernel thermal sysfs ABI (thermal_zone*/temp, trip_point_*).
// sdbus-cpp usage after jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
#include "svc/power/power_backend.h"

#include "acl_config.h"
#include "log/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::power {
namespace {

constexpr std::string_view kTag = "svc.power";

// ── Thermal: kernel thermal zones ───────────────────────────────────────────────
// Each /sys/class/thermal/thermal_zone*/ exposes temp (millidegrees C) and trip
// points (trip_point_N_temp + trip_point_N_type). Android's thermal status is a
// severity ladder, so scale each zone's temperature against its own "critical" trip
// (the vendor-declared shutdown threshold) and take the worst zone. Zones without a
// critical trip fall back to absolute thresholds tuned for CPU/SoC silicon.
std::string read_first_line(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::string s;
  if (in) std::getline(in, s);
  return s;
}

long read_millideg(const std::filesystem::path& p) {
  const std::string s = read_first_line(p);
  if (s.empty()) return 0;
  try { return std::stol(s); } catch (...) { return 0; }
}

// The zone's critical trip temperature in millidegrees, or 0 when it declares none.
long critical_trip(const std::filesystem::path& zone) {
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(zone, ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("trip_point_", 0) != 0 || name.find("_type") == std::string::npos) continue;
    const std::string type = read_first_line(e.path());
    if (type != "critical") continue;
    std::string temp_file = name;
    temp_file.replace(temp_file.find("_type"), 5, "_temp");
    if (const long t = read_millideg(zone / temp_file); t > 0) return t;
  }
  return 0;
}

int thermal_status_from_sysfs() {
  const std::filesystem::path root{"/sys/class/thermal"};
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) return kThermalNone;

  double worst = 0.0;  // fraction of the critical threshold
  for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("thermal_zone", 0) != 0) continue;
    const long temp = read_millideg(e.path() / "temp");
    if (temp <= 0) continue;  // zone with no reading (some wifi/virtual zones)
    const long crit = critical_trip(e.path());
    // No declared critical trip: 100 C stands in as the reference ceiling.
    const double ceiling = crit > 0 ? static_cast<double>(crit) : 100000.0;
    worst = std::max(worst, static_cast<double>(temp) / ceiling);
  }
  if (worst <= 0.0) return kThermalNone;
  if (worst < 0.75) return kThermalNone;
  if (worst < 0.85) return kThermalLight;
  if (worst < 0.92) return kThermalModerate;
  if (worst < 0.97) return kThermalSevere;
  if (worst < 1.00) return kThermalCritical;
  return kThermalEmergency;
}

// ── Null backend ────────────────────────────────────────────────────────────────
// No session bus (or no sdbus): an awake, unthrottled desktop. Wake locks are
// accepted (Android's acquire() never fails) but hold nothing.
class NullBackend : public IPowerBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  AndroidPowerState snapshot() const override {
    AndroidPowerState s;
    s.thermal_status = thermal_status_from_sysfs();  // sysfs needs no bus
    return s;
  }
  void set_sink(std::function<void(const AndroidPowerState&)>) override {}
  long acquire_wake_lock(int, const std::string&) override { return 0; }
  void release_wake_lock(long) override {}
};

#if ACL_HAVE_SDBUS

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

// logind backend: the session's Active/IdleHint/LockedHint give the interactive and
// idle state; power-profiles-daemon's ActiveProfile gives battery-saver; wake locks
// are logind inhibitor fds (held open for the lifetime of the Android wake lock).
class LogindBackend : public IPowerBackend {
 public:
  explicit LogindBackend(std::unique_ptr<sdbus::IConnection> conn) : conn_(std::move(conn)) {
    mgr_ = sdbus::createProxy(*conn_, sdbus::ServiceName{"org.freedesktop.login1"},
                              sdbus::ObjectPath{"/org/freedesktop/login1"});
    // This process's session (the one whose idle/lock state governs our windows).
    sdbus::ObjectPath session{"/org/freedesktop/login1/session/auto"};
    try {
      mgr_->callMethod("GetSessionByPID")
          .onInterface(sdbus::InterfaceName{"org.freedesktop.login1.Manager"})
          .withArguments(static_cast<std::uint32_t>(::getpid()))
          .storeResultsTo(session);
    } catch (const sdbus::Error& e) {
      log::w(kTag, "logind GetSessionByPID failed ({}); using session/auto", e.getMessage());
    }
    session_ = sdbus::createProxy(*conn_, sdbus::ServiceName{"org.freedesktop.login1"}, session);
    session_->uponSignal("PropertiesChanged")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.Properties"})
        .call([this](const std::string&, const std::map<std::string, sdbus::Variant>&,
                     const std::vector<std::string>&) { notify(); });

    // power-profiles-daemon: the modern bus name is org.freedesktop.UPower.PowerProfiles;
    // older builds (and tuned-ppd's compat name) answer on net.hadess.PowerProfiles.
    if (name_has_owner(*conn_, "org.freedesktop.UPower.PowerProfiles")) {
      profiles_ = sdbus::createProxy(*conn_,
                                     sdbus::ServiceName{"org.freedesktop.UPower.PowerProfiles"},
                                     sdbus::ObjectPath{"/org/freedesktop/UPower/PowerProfiles"});
      profiles_iface_ = sdbus::InterfaceName{"org.freedesktop.UPower.PowerProfiles"};
    } else if (name_has_owner(*conn_, "net.hadess.PowerProfiles")) {
      profiles_ = sdbus::createProxy(*conn_, sdbus::ServiceName{"net.hadess.PowerProfiles"},
                                     sdbus::ObjectPath{"/net/hadess/PowerProfiles"});
      profiles_iface_ = sdbus::InterfaceName{"net.hadess.PowerProfiles"};
    }
    if (profiles_) {
      profiles_->uponSignal("PropertiesChanged")
          .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.Properties"})
          .call([this](const std::string&, const std::map<std::string, sdbus::Variant>&,
                       const std::vector<std::string>&) { notify(); });
    }
    conn_->enterEventLoopAsync();
  }
  ~LogindBackend() override {
    try { conn_->leaveEventLoop(); } catch (...) {}
  }

  const char* name() const noexcept override { return "logind"; }

  AndroidPowerState snapshot() const override {
    AndroidPowerState s;
    const sdbus::InterfaceName sess{"org.freedesktop.login1.Session"};
    try {
      const bool active = session_->getProperty("Active").onInterface(sess).get<bool>();
      const bool idle = session_->getProperty("IdleHint").onInterface(sess).get<bool>();
      const bool locked = session_->getProperty("LockedHint").onInterface(sess).get<bool>();
      // The display is usable only when this session is the foreground one and neither
      // the lock screen nor the idle timeout has taken it.
      s.interactive = active && !idle && !locked;
      s.device_idle = idle;
    } catch (const sdbus::Error& e) {
      log::w(kTag, "logind session read failed ({}); assuming interactive", e.getMessage());
    }
    if (profiles_) {
      try {
        const std::string profile =
            profiles_->getProperty("ActiveProfile").onInterface(profiles_iface_).get<std::string>();
        s.power_save = (profile == "power-saver");
      } catch (const sdbus::Error& e) {
        log::w(kTag, "power-profiles read failed ({}); assuming no battery saver", e.getMessage());
      }
    }
    s.thermal_status = thermal_status_from_sysfs();
    return s;
  }

  void set_sink(std::function<void(const AndroidPowerState&)> on_change) override {
    std::lock_guard<std::mutex> lk(sink_mu_);
    sink_ = std::move(on_change);
  }

  // An Android wake lock becomes a logind inhibitor: holding the returned fd blocks
  // the host from suspending (and, for screen-level locks, from idling the display).
  // Closing it releases — so Android's acquire/release maps 1:1 onto host behavior.
  long acquire_wake_lock(int flags, const std::string& tag) override {
    const bool screen = (flags & ~kPartialWakeLock) != 0;  // any SCREEN_*/FULL level
    const std::string what = screen ? "idle:sleep" : "sleep";
    try {
      sdbus::UnixFd fd;
      mgr_->callMethod("Inhibit")
          .onInterface(sdbus::InterfaceName{"org.freedesktop.login1.Manager"})
          .withArguments(what, std::string{"acl"},
                         tag.empty() ? std::string{"Android wake lock"} : tag,
                         std::string{"block"})
          .storeResultsTo(fd);
      std::lock_guard<std::mutex> lk(locks_mu_);
      const long cookie = ++next_cookie_;
      locks_.emplace(cookie, std::move(fd));
      log::i(kTag, "wake lock +{} ({}) -> logind inhibit [{}], {} held", cookie, tag, what,
             locks_.size());
      return cookie;
    } catch (const sdbus::Error& e) {
      log::w(kTag, "logind Inhibit failed ({}); wake lock '{}' not backed", e.getMessage(), tag);
      return 0;
    }
  }

  void release_wake_lock(long cookie) override {
    if (cookie == 0) return;
    std::lock_guard<std::mutex> lk(locks_mu_);
    if (locks_.erase(cookie) != 0)  // UnixFd closes the inhibitor fd on destruction
      log::i(kTag, "wake lock -{} released, {} held", cookie, locks_.size());
  }

 private:
  void notify() {
    std::function<void(const AndroidPowerState&)> sink;
    { std::lock_guard<std::mutex> lk(sink_mu_); sink = sink_; }
    if (sink) sink(snapshot());
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IProxy> mgr_;
  std::unique_ptr<sdbus::IProxy> session_;
  std::unique_ptr<sdbus::IProxy> profiles_;
  sdbus::InterfaceName profiles_iface_{"org.freedesktop.UPower.PowerProfiles"};
  std::mutex sink_mu_;
  std::function<void(const AndroidPowerState&)> sink_;
  std::mutex locks_mu_;
  std::map<long, sdbus::UnixFd> locks_;
  long next_cookie_ = 0;
};

#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<IPowerBackend> make_power_backend() {
#if ACL_HAVE_SDBUS
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSystemBusConnection();
  } catch (const sdbus::Error& e) {
    log::w(kTag, "no system bus ({}); power defaults (null backend)", e.getMessage());
    return std::make_unique<NullBackend>();
  }
  const char* env = std::getenv("ACL_POWER_BACKEND");
  const std::string_view pref = env != nullptr ? env : "";
  if ((pref.empty() || pref == "logind") && name_has_owner(*conn, "org.freedesktop.login1")) {
    log::i(kTag, "power backend: logind");
    return std::make_unique<LogindBackend>(std::move(conn));
  }
  log::i(kTag, "no logind on the bus; power defaults (null backend)");
#endif  // ACL_HAVE_SDBUS
  return std::make_unique<NullBackend>();
}

}  // namespace acl::power

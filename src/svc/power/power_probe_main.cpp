// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool; prints the svc/power backend + derived PowerManager
// state, and exercises a real wake lock. No GPL-3.0 sources.
//
// Prints which host session/power daemons acl selects and the derived
// android.os.PowerManager state, so the logind + power-profiles + thermal-sysfs
// mapping can be eyeballed on desktop/device. With --hold it takes a real wake lock
// (a logind inhibitor) and holds it until Enter, so `systemd-inhibit --list` can
// confirm the inhibitor is actually registered.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "svc/power/power_backend.h"

namespace {
const char* thermal_name(int s) {
  switch (s) {
    case acl::power::kThermalNone: return "none";
    case acl::power::kThermalLight: return "light";
    case acl::power::kThermalModerate: return "moderate";
    case acl::power::kThermalSevere: return "severe";
    case acl::power::kThermalCritical: return "critical";
    case acl::power::kThermalEmergency: return "emergency";
    case acl::power::kThermalShutdown: return "shutdown";
    default: return "?";
  }
}
}  // namespace

int main(int argc, char** argv) {
  auto backend = acl::power::make_power_backend();
  const acl::power::AndroidPowerState s = backend->snapshot();
  std::printf("acl power backend: %s\n", backend->name());
  std::printf("  isInteractive    = %s\n", s.interactive ? "true" : "false");
  std::printf("  isPowerSaveMode  = %s\n", s.power_save ? "true" : "false");
  std::printf("  isDeviceIdleMode = %s\n", s.device_idle ? "true" : "false");
  std::printf("  thermalStatus    = %d (%s)\n", s.thermal_status, thermal_name(s.thermal_status));

  // --hold [seconds]: take a real wake lock. With a seconds argument the hold is timed
  // (scriptable — `systemd-inhibit --list` can be checked meanwhile); without one it
  // waits for Enter.
  const bool hold = argc > 1 && std::strcmp(argv[1], "--hold") == 0;
  if (hold) {
    int seconds = 0;
    if (argc > 2) { try { seconds = std::stoi(argv[2]); } catch (...) { seconds = 0; } }
    const long cookie =
        backend->acquire_wake_lock(acl::power::kPartialWakeLock, "acl-power-probe");
    std::printf("\nwake lock acquired (cookie=%ld)%s\n", cookie,
                cookie != 0 ? " — check: systemd-inhibit --list" : " — NOT backed by an inhibitor");
    if (seconds > 0) {
      std::printf("holding %d s...\n", seconds);
      std::fflush(stdout);
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } else {
      std::printf("press Enter to release...\n");
      std::fflush(stdout);
      (void)std::getchar();
    }
    backend->release_wake_lock(cookie);
    std::printf("released\n");
  }
  return 0;
}

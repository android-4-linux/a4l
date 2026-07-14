// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool; prints the svc/battery backend + derived battery state.
// No GPL-3.0 sources.
//
// Prints which host power daemon acl selects and the derived ACTION_BATTERY_CHANGED
// extras, so the UPower → BatteryManager mapping can be eyeballed on desktop/device.
#include <cstdio>

#include "svc/battery/battery_backend.h"

int main() {
  auto backend = acl::battery::make_battery_backend();
  const acl::battery::AndroidBatteryState s = backend->snapshot();
  std::printf("acl battery backend: %s\n", backend->name());
  std::printf("  level        = %d/%d\n", s.level, s.scale);
  std::printf("  temperature  = %.1f C (%d dC)\n", s.temperature_dc / 10.0, s.temperature_dc);
  std::printf("  voltage      = %d mV\n", s.voltage_mv);
  std::printf("  status       = %d  (2 chg, 3 dischg, 4 not-chg, 5 full)\n", s.status);
  std::printf("  health       = %d  (2 good)\n", s.health);
  std::printf("  plugged      = %d  (0 none, 1 AC, 2 USB, 4 wireless)\n", s.plugged);
  std::printf("  present      = %s\n", s.present ? "true" : "false");
  std::printf("  technology   = %s\n", s.technology.c_str());
  return 0;
}

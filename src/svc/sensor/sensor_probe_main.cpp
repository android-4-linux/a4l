// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl dev tool (mirrors battery_probe_main). Exercises the
// sensor hub end to end (enumerate + enable + receive samples) with no JVM —
// the "one good example" demo and a smoke check that the poll thread +
// SensorSource delivery works. No GPL-3.0 sources.

#include <chrono>
#include <cstdio>
#include <thread>

#include "svc/sensor/sensor.h"

using namespace acl::svc::sensor;

int main() {
  SensorHub& hub = SensorHub::instance();
  std::printf("sensors: %zu\n", hub.sensors().size());
  for (const SensorDesc& d : hub.sensors())
    std::printf(
        "  handle=%d type=%d \"%s\" (%s) range=%.2f res=%.4f minDelayUs=%d "
        "values=%d\n",
        d.handle, d.type, d.name.c_str(), d.vendor.c_str(), d.max_range,
        d.resolution, d.min_delay_us, d.value_count);
  if (hub.sensors().empty()) {
    std::printf("no sensors\n");
    return 1;
  }

  const int handle = hub.sensors()[0].handle;
  int count = 0;
  hub.set_listener([&](const SensorSample& s) {
    if (count < 5)
      std::printf("  sample handle=%d [% .3f % .3f % .3f] acc=%d ts=%lld\n",
                  s.handle, s.values[0], s.values[1], s.values[2], s.accuracy,
                  static_cast<long long>(s.timestamp_ns));
    ++count;
  });
  std::printf("enable handle=%d @ 50Hz for 300ms...\n", handle);
  hub.enable(handle, 20000);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  hub.disable(handle);
  std::printf("received %d sample(s)\n", count);
  return count > 0 ? 0 : 1;
}

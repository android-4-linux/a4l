// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl dev tool (mirrors svc/sensor/sensor_probe_main.cpp). Exercises the
// acl::svc::location hub (GeoClue2 / synthetic) with no JVM — the "one good example working". No GPL-3.0 sources.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "svc/location/location.h"

// acl-location-probe: start the location hub and print fixes as they arrive. Backed by host
// GeoClue2 when available, or a fixed point via ACL_LOCATION_LATLON="lat,lon[,accM]". Exits after
// a few fixes or a timeout — enough to prove the source picks and delivers.
int main() {
  using namespace acl::svc::location;

  std::atomic<int> count{0};
  LocationHub::instance().set_listener([&count](const LocationFix& f) {
    std::printf("fix #%d: lat=%.6f lon=%.6f acc=%.1fm%s%s%s t=%lldms\n", ++count, f.latitude,
                f.longitude, f.accuracy_m,
                f.has_altitude ? "" : " (no-alt)", f.has_speed ? "" : " (no-speed)",
                f.has_bearing ? "" : " (no-bearing)",
                static_cast<long long>(f.timestamp_ms));
    std::fflush(stdout);
  });

  const bool delivering = LocationHub::instance().start();
  std::printf("source=%s available=%d delivering=%d\n", LocationHub::instance().source_name(),
              LocationHub::instance().available(), delivering);
  std::fflush(stdout);

  // Wait up to ~10s for a few fixes (GeoClue may take a moment to resolve the first one).
  for (int i = 0; i < 100 && count.load() < 3; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

  LocationHub::instance().stop();
  std::printf("done (%d fix(es))\n", count.load());
  return count.load() > 0 ? 0 : 1;
}

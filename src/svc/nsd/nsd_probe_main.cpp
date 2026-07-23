// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool; browses a service type via the svc/nsd backend and
// prints discoveries + resolutions. No GPL-3.0 sources.
//
// Usage: acl-nsd-probe [_service._tcp] [seconds]
// Verifies the Avahi -> NsdManager mapping end to end: every browsed instance is
// resolved, so host/port/TXT come back exactly as NsdManager would report them.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "svc/nsd/nsd_backend.h"

int main(int argc, char** argv) {
  const std::string type = argc > 1 ? argv[1] : "_googlecast._tcp";
  int seconds = 5;
  if (argc > 2) { try { seconds = std::stoi(argv[2]); } catch (...) { seconds = 5; } }

  auto backend = acl::nsd::make_nsd_backend();
  std::printf("acl nsd backend: %s (available=%s)\n", backend->name(),
              backend->available() ? "yes" : "no");
  if (!backend->available()) return 0;

  backend->set_sinks(
      [&backend](long h, acl::nsd::DiscoveryEvent ev, const acl::nsd::NsdService& s) {
        std::printf("  [%s] %s  (%s in %s)  handle=%ld\n",
                    ev == acl::nsd::DiscoveryEvent::kFound ? "FOUND" : "LOST",
                    s.name.c_str(), s.type.c_str(), s.domain.c_str(), h);
        if (ev == acl::nsd::DiscoveryEvent::kFound)
          backend->resolve(s.name, s.type, s.domain);
      },
      [](bool ok, const acl::nsd::NsdService& s) {
        if (!ok) { std::printf("  [RESOLVE FAILED]\n"); return; }
        std::printf("  [RESOLVED] %s -> %s (%s) port %d\n", s.name.c_str(), s.host.c_str(),
                    s.address.c_str(), s.port);
        for (const auto& [k, v] : s.txt)
          std::printf("      txt %s=%s\n", k.c_str(), v.c_str());
      });

  std::printf("browsing %s for %d s...\n", type.c_str(), seconds);
  const long h = backend->start_discovery(type);
  if (h == 0) { std::printf("failed to start discovery\n"); return 1; }
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  backend->stop_discovery(h);
  std::printf("done\n");
  return 0;
}

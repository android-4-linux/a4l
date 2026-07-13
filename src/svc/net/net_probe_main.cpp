// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool. Selects the svc/net connectivity backend that owns the
// host D-Bus bus and prints the derived AndroidNetState. No GPL-3.0 sources.
//
// Run on any target to see which daemon acl picks and how its state maps to
// android.net.ConnectivityManager: `acl-net-probe` (honors $ACL_NET_BACKEND).
#include "svc/net/net_backend.h"

#include <cstdio>

int main() {
  auto backend = acl::net::make_net_backend();
  const acl::net::AndroidNetState s = backend->snapshot();

  auto transport = [](acl::net::AndroidNetState::Transport t) {
    switch (t) {
      case acl::net::AndroidNetState::Transport::kWifi: return "WIFI";
      case acl::net::AndroidNetState::Transport::kEthernet: return "ETHERNET";
      case acl::net::AndroidNetState::Transport::kCellular: return "CELLULAR";
      default: return "NONE";
    }
  };

  std::printf("backend   : %s\n", backend->name());
  std::printf("connected : %s   (getActiveNetwork() != null)\n", s.connected ? "yes" : "no");
  std::printf("validated : %s   (NET_CAPABILITY_VALIDATED)\n", s.validated ? "yes" : "no");
  std::printf("metered   : %s   (!NET_CAPABILITY_NOT_METERED)\n", s.metered ? "yes" : "no");
  std::printf("transport : %s\n", transport(s.transport));
  return 0;
}

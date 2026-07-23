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
  const acl::net::AndroidWifiState w = backend->wifi_snapshot();
  std::printf("\n-- WifiManager (android.net.wifi) --\n");
  std::printf("enabled   : %s   (isWifiEnabled())\n", w.enabled ? "yes" : "no");
  std::printf("connected : %s   (associated with an AP)\n", w.connected ? "yes" : "no");
  std::printf("ssid      : %s\n", w.ssid.empty() ? "(none)" : w.ssid.c_str());
  std::printf("bssid     : %s\n", w.bssid.empty() ? "(none)" : w.bssid.c_str());
  std::printf("rssi      : %d dBm\n", w.rssi_dbm);
  std::printf("linkSpeed : %d Mbps\n", w.link_speed_mbps);
  std::printf("frequency : %d MHz\n", w.frequency_mhz);
  return 0;
}

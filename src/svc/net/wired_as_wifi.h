// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l; android.net.wifi.WifiInfo's public documentation
// (the hardware address an app with no permission is given). No GPL-3.0
// sources.
//
// ACL_NET_WIRED_AS_WIFI: the host's wired network, shown to the app as a Wi-Fi
// one. An app written for a phone may do its work on the local network only
// when it believes it is on Wi-Fi: it looks for its speakers, its cast targets
// or its printer when ConnectivityManager says TRANSPORT_WIFI and WifiManager
// names a network, and not otherwise. A board on Ethernet reaches the same
// devices, and the app never looks.
//
// Off unless asked for: what a4l reports is the host's state, and this is not.
#ifndef ACL_SVC_NET_WIRED_AS_WIFI_H
#define ACL_SVC_NET_WIRED_AS_WIFI_H

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "svc/net/net_backend.h"

namespace acl::net {

// The network name given when ACL_NET_WIRED_AS_WIFI does not name one.
inline constexpr std::string_view kWiredAsWifiDefaultSsid = "Wired";

// What ACL_NET_WIRED_AS_WIFI asks for.
struct WiredAsWifi {
  bool enabled = false;
  std::string ssid;  // WifiInfo.getSSID() for the wired network
};

// Unset, empty, "0", "no", "off" and "false" ask for nothing. "1", "yes", "on"
// and "true" ask for the default name. Anything else is the name to give.
inline WiredAsWifi parse_wired_as_wifi(const char* value) {
  const std::string_view v = value != nullptr ? value : "";
  if (v.empty() || v == "0" || v == "no" || v == "off" || v == "false")
    return {};
  if (v == "1" || v == "yes" || v == "on" || v == "true")
    return {true, std::string{kWiredAsWifiDefaultSsid}};
  return {true, std::string{v}};
}

// The connectivity state the app is shown: an active wired network is a Wi-Fi
// one. Anything else is as the host has it.
inline AndroidNetState wired_as_wifi(AndroidNetState s) {
  if (s.connected && s.transport == AndroidNetState::Transport::kEthernet)
    s.transport = AndroidNetState::Transport::kWifi;
  return s;
}

// The Wi-Fi state the app is shown, from the host's connectivity and Wi-Fi
// states. An access point the host is really associated with is reported as it
// is. Failing that, an active wired network is a connected Wi-Fi network named
// `ssid`: a good signal, the hardware address WifiInfo gives an app that may
// not see the real one, and no link speed or frequency, which it does not
// have.
inline AndroidWifiState wired_as_wifi(const AndroidNetState& net,
                                      AndroidWifiState host,
                                      std::string_view ssid) {
  if (host.connected)
    return host;
  if (!net.connected || net.transport != AndroidNetState::Transport::kEthernet)
    return host;
  AndroidWifiState w;
  w.enabled = true;
  w.connected = true;
  w.ssid = ssid;
  w.bssid = "02:00:00:00:00:00";
  w.rssi_dbm = -50;
  return w;
}

// A backend that shows the app another backend's wired network as Wi-Fi.
class WiredAsWifiBackend final : public INetBackend {
 public:
  WiredAsWifiBackend(std::unique_ptr<INetBackend> host, std::string ssid)
      : host_(std::move(host)), ssid_(std::move(ssid)) {}

  const char* name() const noexcept override { return host_->name(); }

  AndroidNetState snapshot() const override {
    return wired_as_wifi(host_->snapshot());
  }

  // The host's sink is told of every change of the host's state. Two of them
  // can be one state here, wired and then Wi-Fi, and the app is told once.
  void set_sink(
      std::function<void(const AndroidNetState&)> on_change) override {
    if (!on_change) {
      host_->set_sink(nullptr);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      shown_ = snapshot();
    }
    host_->set_sink(
        [this, cb = std::move(on_change)](const AndroidNetState& host) {
          const AndroidNetState s = wired_as_wifi(host);
          {
            std::lock_guard<std::mutex> lk(m_);
            if (s == shown_)
              return;
            shown_ = s;
          }
          cb(s);
        });
  }

  AndroidWifiState wifi_snapshot() const override {
    return wired_as_wifi(host_->snapshot(), host_->wifi_snapshot(), ssid_);
  }

 private:
  std::unique_ptr<INetBackend> host_;
  std::string ssid_;
  std::mutex m_;
  AndroidNetState shown_;  // what the sink was last told, or would have been
};

}  // namespace acl::net

#endif  // ACL_SVC_NET_WIRED_AS_WIFI_H

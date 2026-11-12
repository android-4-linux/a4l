// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.net.ConnectivityManager/NetworkCapabilities
// public surface (AOSP android.jar stubs, Apache-2.0); host mapping from
// freedesktop systemd-networkd (org.freedesktop.network1), NetworkManager, and
// net.connman D-Bus API docs; sdbus-cpp usage pattern after
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
#include "svc/net/net_backend.h"

#include "acl_config.h"
#include "log/log.h"

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#include <map>
#include <vector>
#endif

namespace acl::net {
namespace {

using Transport = AndroidNetState::Transport;
constexpr std::string_view kTag = "svc.net";

// ---- Null backend: no daemon on the bus → permanently offline (the prior
// stub) ----
class NullBackend final : public INetBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  AndroidNetState snapshot() const override { return {}; }
  void set_sink(std::function<void(const AndroidNetState&)>) override {}
};

#if ACL_HAVE_SDBUS
// ---- shared base: caches state, dedups, fans out to the sink ----
class DbusBackend : public INetBackend {
 public:
  explicit DbusBackend(std::unique_ptr<sdbus::IConnection> conn)
      : conn_(std::move(conn)) {}

  AndroidNetState snapshot() const override {
    std::lock_guard<std::mutex> lk(m_);
    return state_;
  }
  void set_sink(std::function<void(const AndroidNetState&)> cb) override {
    std::lock_guard<std::mutex> lk(m_);
    sink_ = std::move(cb);
  }

 protected:
  // Store the new state; fire the sink only when it actually changed.
  void publish(const AndroidNetState& s) {
    std::function<void(const AndroidNetState&)> cb;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (s == state_)
        return;
      state_ = s;
      cb = sink_;
    }
    if (cb)
      cb(s);
  }
  // Subscribe to org.freedesktop.DBus.Properties.PropertiesChanged → refresh().
  void watch_properties(const std::function<void()>& refresh) {
    proxy_->uponSignal("PropertiesChanged")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.Properties"})
        .call([refresh](const std::string&,
                        const std::map<std::string, sdbus::Variant>&,
                        const std::vector<std::string>&) { refresh(); });
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IProxy> proxy_;

 private:
  mutable std::mutex m_;
  AndroidNetState state_;
  std::function<void(const AndroidNetState&)> sink_;
};
#endif  // ACL_HAVE_SDBUS

#if ACL_NET_NETWORKMANAGER
// org.freedesktop.NetworkManager — richest source: State/Connectivity/Metered
// are first-class, PrimaryConnectionType names the transport directly.
class NetworkManagerBackend final : public DbusBackend {
 public:
  explicit NetworkManagerBackend(std::unique_ptr<sdbus::IConnection> conn)
      : DbusBackend(std::move(conn)) {
    proxy_ = sdbus::createProxy(
        *conn_, sdbus::ServiceName{"org.freedesktop.NetworkManager"},
        sdbus::ObjectPath{"/org/freedesktop/NetworkManager"});
    watch_properties([this] { refresh(); });
    refresh();
    conn_->enterEventLoopAsync();
  }
  const char* name() const noexcept override { return "networkmanager"; }

  // WifiManager/WifiInfo: the radio switch is NetworkManager's WirelessEnabled;
  // the association details come from the first NM_DEVICE_TYPE_WIFI device's
  // ActiveAccessPoint (SSID/BSSID/strength/frequency/bitrate).
  AndroidWifiState wifi_snapshot() const override {
    AndroidWifiState w;
    const sdbus::InterfaceName nm{"org.freedesktop.NetworkManager"};
    try {
      w.enabled =
          proxy_->getProperty("WirelessEnabled").onInterface(nm).get<bool>();
    } catch (const sdbus::Error&) {
      return w;  // no wireless support on this daemon build
    }
    if (!w.enabled)
      return w;  // radio off: nothing else is meaningful

    try {
      std::vector<sdbus::ObjectPath> devices;
      proxy_->callMethod("GetDevices").onInterface(nm).storeResultsTo(devices);
      for (const auto& dpath : devices) {
        auto dev = sdbus::createProxy(
            *conn_, sdbus::ServiceName{"org.freedesktop.NetworkManager"},
            dpath);
        std::uint32_t dtype = 0;
        try {
          dtype = dev->getProperty("DeviceType")
                      .onInterface(sdbus::InterfaceName{
                          "org.freedesktop.NetworkManager.Device"})
                      .get<std::uint32_t>();
        } catch (const sdbus::Error&) {
          continue;
        }
        if (dtype != 2)
          continue;  // NM_DEVICE_TYPE_WIFI

        sdbus::ObjectPath ap;
        try {
          ap = dev->getProperty("ActiveAccessPoint")
                   .onInterface(sdbus::InterfaceName{
                       "org.freedesktop.NetworkManager.Device.Wireless"})
                   .get<sdbus::ObjectPath>();
        } catch (const sdbus::Error&) {
          continue;
        }
        if (ap.empty() || ap == "/")
          return w;  // radio on but not associated

        auto apx = sdbus::createProxy(
            *conn_, sdbus::ServiceName{"org.freedesktop.NetworkManager"}, ap);
        const sdbus::InterfaceName api{
            "org.freedesktop.NetworkManager.AccessPoint"};
        try {
          const std::vector<std::uint8_t> ssid =
              apx->getProperty("Ssid")
                  .onInterface(api)
                  .get<std::vector<std::uint8_t>>();
          w.ssid.assign(ssid.begin(), ssid.end());
          w.bssid =
              apx->getProperty("HwAddress").onInterface(api).get<std::string>();
          const std::uint8_t strength =
              apx->getProperty("Strength").onInterface(api).get<std::uint8_t>();
          w.frequency_mhz = static_cast<int>(apx->getProperty("Frequency")
                                                 .onInterface(api)
                                                 .get<std::uint32_t>());
          const std::uint32_t maxbr = apx->getProperty("MaxBitrate")
                                          .onInterface(api)
                                          .get<std::uint32_t>();
          // NM reports signal quality as 0..100%; Android wants dBm. The
          // conventional linear fit maps 0% -> -100 dBm and 100% -> -50 dBm.
          w.rssi_dbm = static_cast<int>(strength) / 2 - 100;
          w.link_speed_mbps = maxbr > 0 ? static_cast<int>(maxbr / 1000)
                                        : -1;  // kbit/s -> Mbps
          w.connected = true;
        } catch (const sdbus::Error& e) {
          acl::log::w(kTag, "NetworkManager access-point read failed: {}",
                      e.getMessage());
        }
        return w;
      }
    } catch (const sdbus::Error& e) {
      acl::log::w(kTag, "NetworkManager wifi device enumeration failed: {}",
                  e.getMessage());
    }
    return w;
  }

 private:
  void refresh() {
    const sdbus::InterfaceName i{"org.freedesktop.NetworkManager"};
    AndroidNetState s;
    try {
      uint32_t state =
          proxy_->getProperty("State").onInterface(i).get<uint32_t>();
      uint32_t conny =
          proxy_->getProperty("Connectivity").onInterface(i).get<uint32_t>();
      uint32_t metered =
          proxy_->getProperty("Metered").onInterface(i).get<uint32_t>();
      std::string ptype = proxy_->getProperty("PrimaryConnectionType")
                              .onInterface(i)
                              .get<std::string>();
      s.connected = (state == 70);                 // NM_STATE_CONNECTED_GLOBAL
      s.validated = (conny == 4);                  // NM_CONNECTIVITY_FULL
      s.metered = (metered == 1 || metered == 3);  // YES / GUESS_YES
      if (ptype == "802-11-wireless")
        s.transport = Transport::kWifi;
      else if (ptype == "802-3-ethernet")
        s.transport = Transport::kEthernet;
      else if (ptype == "gsm" || ptype == "cdma")
        s.transport = Transport::kCellular;
    } catch (const sdbus::Error& e) {
      acl::log::w(kTag, "NetworkManager refresh failed: {}", e.getMessage());
    }
    publish(s);
  }
};
#endif  // ACL_NET_NETWORKMANAGER

#if ACL_NET_NETWORKD
// org.freedesktop.network1 (systemd-networkd). Thinner API: OperationalState
// gives reachability; there is no portal check (validated≈routable) and no
// metered notion (both documented fidelity gaps). Transport is classified from
// the link name.
class NetworkdBackend final : public DbusBackend {
 public:
  explicit NetworkdBackend(std::unique_ptr<sdbus::IConnection> conn)
      : DbusBackend(std::move(conn)) {
    proxy_ = sdbus::createProxy(*conn_,
                                sdbus::ServiceName{"org.freedesktop.network1"},
                                sdbus::ObjectPath{"/org/freedesktop/network1"});
    watch_properties([this] { refresh(); });
    refresh();
    conn_->enterEventLoopAsync();
  }
  const char* name() const noexcept override { return "networkd"; }

 private:
  void refresh() {
    const sdbus::InterfaceName i{"org.freedesktop.network1.Manager"};
    AndroidNetState s;
    try {
      std::string op = proxy_->getProperty("OperationalState")
                           .onInterface(i)
                           .get<std::string>();
      std::string online;
      try {  // OnlineState is systemd v249+; tolerate its absence.
        online = proxy_->getProperty("OnlineState")
                     .onInterface(i)
                     .get<std::string>();
      } catch (...) {
      }
      s.connected = (online == "online") || (op == "routable");
      s.validated =
          s.connected;    // no reachability check in networkd (fidelity gap)
      s.metered = false;  // networkd has no metered concept (fidelity gap)
      s.transport = active_transport(i);
    } catch (const sdbus::Error& e) {
      acl::log::w(kTag, "networkd refresh failed: {}", e.getMessage());
    }
    publish(s);
  }
  // ListLinks() → a(iso); classify the first non-loopback link by its
  // (predictable) interface name. Heuristic — a documented non-optimal bucket;
  // a Link.Describe() JSON read would be exact.
  Transport active_transport(const sdbus::InterfaceName& i) {
    try {
      std::vector<sdbus::Struct<int32_t, std::string, sdbus::ObjectPath>> links;
      proxy_->callMethod("ListLinks").onInterface(i).storeResultsTo(links);
      for (const auto& l : links) {
        const std::string& n = std::get<1>(l);
        if (n == "lo")
          continue;
        if (n.rfind("wl", 0) == 0)
          return Transport::kWifi;  // wlan0, wlp*
        if (n.rfind("ww", 0) == 0)
          return Transport::kCellular;  // wwan0 (modem)
        return Transport::kEthernet;    // en*, eth*, ...
      }
    } catch (const sdbus::Error& e) {
      acl::log::w(kTag, "networkd ListLinks failed: {}", e.getMessage());
    }
    return Transport::kNone;
  }
};
#endif  // ACL_NET_NETWORKD

#if ACL_NET_CONNMAN
// net.connman — the device/automotive default. State "online" already implies a
// verified internet path (ConnMan runs its own connectivity check) → validated.
// No first-class metered property, so cellular ⇒ metered (documented gap).
class ConnmanBackend final : public DbusBackend {
 public:
  explicit ConnmanBackend(std::unique_ptr<sdbus::IConnection> conn)
      : DbusBackend(std::move(conn)) {
    proxy_ = sdbus::createProxy(*conn_, sdbus::ServiceName{"net.connman"},
                                sdbus::ObjectPath{"/"});
    proxy_->uponSignal("PropertyChanged")
        .onInterface(sdbus::InterfaceName{"net.connman.Manager"})
        .call([this](const std::string&, const sdbus::Variant&) { refresh(); });
    proxy_->uponSignal("ServicesChanged")
        .onInterface(sdbus::InterfaceName{"net.connman.Manager"})
        .call(
            [this](
                const std::vector<sdbus::Struct<
                    sdbus::ObjectPath, std::map<std::string, sdbus::Variant>>>&,
                const std::vector<sdbus::ObjectPath>&) { refresh(); });
    refresh();
    conn_->enterEventLoopAsync();
  }
  const char* name() const noexcept override { return "connman"; }

 private:
  void refresh() {
    const sdbus::InterfaceName mi{"net.connman.Manager"};
    AndroidNetState s;
    try {
      std::map<std::string, sdbus::Variant> props;
      proxy_->callMethod("GetProperties").onInterface(mi).storeResultsTo(props);
      std::string state =
          props.count("State") ? props["State"].get<std::string>() : "";
      s.connected = (state == "ready" || state == "online");
      s.validated = (state == "online");

      // GetServices() → a(oa{sv}); the first entry is the default/active
      // service.
      std::vector<sdbus::Struct<sdbus::ObjectPath,
                                std::map<std::string, sdbus::Variant>>>
          svcs;
      proxy_->callMethod("GetServices").onInterface(mi).storeResultsTo(svcs);
      if (!svcs.empty()) {
        auto& sp = std::get<1>(svcs.front());
        std::string type =
            sp.count("Type") ? sp["Type"].get<std::string>() : "";
        if (type == "wifi")
          s.transport = Transport::kWifi;
        else if (type == "ethernet" || type == "gadget")
          s.transport = Transport::kEthernet;
        else if (type == "cellular") {
          s.transport = Transport::kCellular;
          s.metered = true;
        }
      }
    } catch (const sdbus::Error& e) {
      acl::log::w(kTag, "connman refresh failed: {}", e.getMessage());
    }
    publish(s);
  }
};
#endif  // ACL_NET_CONNMAN

#if ACL_HAVE_SDBUS
// True iff `bus_name` currently has an owner (a running daemon) on the bus.
bool name_has_owner(sdbus::IConnection& conn, const char* bus_name) {
  try {
    auto dbus =
        sdbus::createProxy(conn, sdbus::ServiceName{"org.freedesktop.DBus"},
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
#endif

}  // namespace

std::unique_ptr<INetBackend> make_net_backend() {
#if ACL_HAVE_SDBUS
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSystemBusConnection();
  } catch (const sdbus::Error& e) {
    acl::log::w(kTag, "no system bus ({}); connectivity offline",
                e.getMessage());
    return std::make_unique<NullBackend>();
  }

  const char* env = std::getenv("ACL_NET_BACKEND");
  const std::string_view pref = env ? env : "";

  // Probe with sync calls (no event loop needed); the chosen backend takes the
  // connection and starts its own async loop. Order: automotive/device ConnMan,
  // then NetworkManager, then systemd-networkd — an explicit override still
  // wins.
#if ACL_NET_CONNMAN
  if ((pref.empty() || pref == "connman") &&
      name_has_owner(*conn, "net.connman")) {
    acl::log::i(kTag, "connectivity backend: connman");
    return std::make_unique<ConnmanBackend>(std::move(conn));
  }
#endif
#if ACL_NET_NETWORKMANAGER
  if ((pref.empty() || pref == "networkmanager") &&
      name_has_owner(*conn, "org.freedesktop.NetworkManager")) {
    acl::log::i(kTag, "connectivity backend: networkmanager");
    return std::make_unique<NetworkManagerBackend>(std::move(conn));
  }
#endif
#if ACL_NET_NETWORKD
  if ((pref.empty() || pref == "networkd") &&
      name_has_owner(*conn, "org.freedesktop.network1")) {
    acl::log::i(kTag, "connectivity backend: networkd");
    return std::make_unique<NetworkdBackend>(std::move(conn));
  }
#endif
  acl::log::i(
      kTag,
      "no supported connectivity daemon on the bus; offline (null backend)");
#endif  // ACL_HAVE_SDBUS
  return std::make_unique<NullBackend>();
}

}  // namespace acl::net

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test code. No GPL-3.0 sources.
//
// ACL_NET_WIRED_AS_WIFI: an active wired network is shown to the app as a
// connected Wi-Fi one, and nothing else is changed.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "svc/net/wired_as_wifi.h"

using acl::net::AndroidNetState;
using acl::net::AndroidWifiState;
using acl::net::parse_wired_as_wifi;
using acl::net::WiredAsWifiBackend;
using Transport = acl::net::AndroidNetState::Transport;

namespace {

// A host whose state the test sets, and whose changes it reports.
struct FakeHost final : acl::net::INetBackend {
  AndroidNetState net;
  AndroidWifiState wifi;
  std::function<void(const AndroidNetState&)> sink;

  const char* name() const noexcept override { return "fake"; }
  AndroidNetState snapshot() const override { return net; }
  void set_sink(std::function<void(const AndroidNetState&)> cb) override {
    sink = std::move(cb);
  }
  AndroidWifiState wifi_snapshot() const override { return wifi; }

  void change(const AndroidNetState& s) {
    net = s;
    if (sink)
      sink(s);
  }
};

AndroidNetState online(Transport t) {
  AndroidNetState s;
  s.connected = true;
  s.validated = true;
  s.transport = t;
  return s;
}

struct Shown {
  FakeHost* host;  // owned by `backend`
  std::unique_ptr<WiredAsWifiBackend> backend;
};

Shown shown_as(const char* ssid = "Wired") {
  auto host = std::make_unique<FakeHost>();
  FakeHost* raw = host.get();
  return {raw, std::make_unique<WiredAsWifiBackend>(std::move(host), ssid)};
}

}  // namespace

TEST_CASE("ACL_NET_WIRED_AS_WIFI is off unless it asks", "[net][wifi]") {
  for (const char* off : {"", "0", "no", "off", "false"})
    CHECK_FALSE(parse_wired_as_wifi(off).enabled);
  CHECK_FALSE(parse_wired_as_wifi(nullptr).enabled);

  for (const char* on : {"1", "yes", "on", "true"}) {
    const auto w = parse_wired_as_wifi(on);
    CHECK(w.enabled);
    CHECK(w.ssid == "Wired");
  }
  const auto named = parse_wired_as_wifi("Home network");
  CHECK(named.enabled);
  CHECK(named.ssid == "Home network");
}

TEST_CASE("an active wired network is shown as a Wi-Fi one", "[net][wifi]") {
  auto s = shown_as("Home");
  s.host->net = online(Transport::kEthernet);

  const AndroidNetState net = s.backend->snapshot();
  CHECK(net.connected);
  CHECK(net.validated);
  CHECK_FALSE(net.metered);
  CHECK(net.transport == Transport::kWifi);

  const AndroidWifiState wifi = s.backend->wifi_snapshot();
  CHECK(wifi.enabled);
  CHECK(wifi.connected);
  CHECK(wifi.ssid == "Home");
  CHECK(wifi.bssid == "02:00:00:00:00:00");
  CHECK(wifi.rssi_dbm == -50);
  CHECK(wifi.link_speed_mbps == -1);
  CHECK(wifi.frequency_mhz == 0);
  CHECK(std::string_view{s.backend->name()} == "fake");
}

TEST_CASE("a host that is not on a wired network is shown as it is",
          "[net][wifi]") {
  auto s = shown_as();

  // Offline, the radio off.
  CHECK(s.backend->snapshot() == AndroidNetState{});
  CHECK(s.backend->wifi_snapshot() == AndroidWifiState{});

  // A cable in with no address yet.
  AndroidNetState cable;
  cable.transport = Transport::kEthernet;
  s.host->net = cable;
  CHECK(s.backend->snapshot() == cable);
  CHECK_FALSE(s.backend->wifi_snapshot().connected);

  s.host->net = online(Transport::kCellular);
  CHECK(s.backend->snapshot().transport == Transport::kCellular);
  CHECK_FALSE(s.backend->wifi_snapshot().connected);

  // On Wi-Fi of its own.
  AndroidWifiState real;
  real.enabled = true;
  real.connected = true;
  real.ssid = "Real";
  real.bssid = "aa:bb:cc:dd:ee:ff";
  real.rssi_dbm = -61;
  real.frequency_mhz = 5180;
  s.host->net = online(Transport::kWifi);
  s.host->wifi = real;
  CHECK(s.backend->snapshot() == online(Transport::kWifi));
  CHECK(s.backend->wifi_snapshot() == real);
}

TEST_CASE("the host's own access point is the one named", "[net][wifi]") {
  // Wired is the route in use, and the host is on an access point as well.
  auto s = shown_as("Home");
  AndroidWifiState real;
  real.enabled = true;
  real.connected = true;
  real.ssid = "Real";
  s.host->net = online(Transport::kEthernet);
  s.host->wifi = real;

  CHECK(s.backend->snapshot().transport == Transport::kWifi);
  CHECK(s.backend->wifi_snapshot() == real);

  // The radio on with no access point: the wired network is the one named.
  real.connected = false;
  real.ssid.clear();
  s.host->wifi = real;
  CHECK(s.backend->wifi_snapshot().ssid == "Home");
}

TEST_CASE("a change is passed on as the app is shown it, and once",
          "[net][wifi]") {
  auto s = shown_as();
  std::vector<AndroidNetState> told;
  s.backend->set_sink([&told](const AndroidNetState& n) { told.push_back(n); });

  s.host->change(online(Transport::kEthernet));
  REQUIRE(told.size() == 1);
  CHECK(told[0].transport == Transport::kWifi);

  // Wired to Wi-Fi is no change to the app.
  s.host->change(online(Transport::kWifi));
  CHECK(told.size() == 1);

  s.host->change(AndroidNetState{});
  REQUIRE(told.size() == 2);
  CHECK_FALSE(told[1].connected);

  s.backend->set_sink(nullptr);
  CHECK_FALSE(s.host->sink);
}

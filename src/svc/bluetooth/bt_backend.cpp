// SPDX-License-Identifier: Apache-2.0
// Provenance: BlueZ org.bluez Adapter1/Device1 + org.freedesktop.DBus.ObjectManager
// D-Bus API docs → android.bluetooth.BluetoothAdapter/BluetoothDevice (AOSP android.jar
// stubs, Apache-2.0). sdbus-c++ + cached-object-model pattern after
// jwinarske/sdbus-cpp-examples/src/bluez and jwinarske/bluez_native (Apache-2.0). No GPL-3.0 sources.
#include "svc/bluetooth/bt_backend.h"

#include "acl_config.h"
#include "log/log.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::bt {
namespace {

constexpr std::string_view kTag = "svc.bluetooth";

// The Null backend: no BlueZ (or no sdbus). No adapter → STATE_OFF, empty bonded set.
class NullBackend : public IBtBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  AndroidBtState snapshot() const override { return {}; }
  bool set_powered(bool) override { return false; }
  bool start_discovery() override { return false; }
  bool stop_discovery() override { return false; }
  void set_sink(std::function<void(const AndroidBtState&)>) override {}
};

#if ACL_HAVE_SDBUS

using PropMap = std::map<std::string, sdbus::Variant>;
using IfaceMap = std::map<std::string, PropMap>;
using ManagedObjects = std::map<sdbus::ObjectPath, IfaceMap>;

bool get_bool(const PropMap& p, const char* k) {
  auto it = p.find(k);
  if (it == p.end()) return false;
  try { return it->second.get<bool>(); } catch (...) { return false; }
}
std::string get_str(const PropMap& p, const char* k) {
  auto it = p.find(k);
  if (it == p.end()) return {};
  try { return it->second.get<std::string>(); } catch (...) { return {}; }
}
std::uint32_t get_u32(const PropMap& p, const char* k) {
  auto it = p.find(k);
  if (it == p.end()) return 0;
  try { return it->second.get<std::uint32_t>(); } catch (...) { return 0; }
}

bool name_has_owner(sdbus::IConnection& conn, const char* bus_name) {
  try {
    auto dbus = sdbus::createProxy(conn, sdbus::ServiceName{"org.freedesktop.DBus"},
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

// BlueZ backend: snapshot the object tree via ObjectManager.GetManagedObjects into a small
// cached AndroidBtState (first Adapter1 + all Paired Device1), rebuilt on InterfacesAdded/
// Removed and the adapter's PropertiesChanged. Synchronous Android reads answer off the cache;
// only mutations (Powered / StartDiscovery / StopDiscovery) call BlueZ.
class BlueZBackend : public IBtBackend {
 public:
  explicit BlueZBackend(std::unique_ptr<sdbus::IConnection> conn) : conn_(std::move(conn)) {
    om_ = sdbus::createProxy(*conn_, sdbus::ServiceName{"org.bluez"}, sdbus::ObjectPath{"/"});
    om_->uponSignal("InterfacesAdded")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.ObjectManager"})
        .call([this](const sdbus::ObjectPath&, const IfaceMap&) { rebuild(); });
    om_->uponSignal("InterfacesRemoved")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.ObjectManager"})
        .call([this](const sdbus::ObjectPath&, const std::vector<std::string>&) { rebuild(); });
    rebuild();
    conn_->enterEventLoopAsync();
  }
  ~BlueZBackend() override {
    try { conn_->leaveEventLoop(); } catch (...) {}
  }

  const char* name() const noexcept override { return "bluez"; }

  AndroidBtState snapshot() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
  }

  bool set_powered(bool on) override {
    return with_adapter([on](sdbus::IProxy& a) {
      a.setProperty("Powered").onInterface(sdbus::InterfaceName{"org.bluez.Adapter1"}).toValue(on);
    });
  }
  bool start_discovery() override {
    return with_adapter([](sdbus::IProxy& a) {
      a.callMethod("StartDiscovery").onInterface(sdbus::InterfaceName{"org.bluez.Adapter1"});
    });
  }
  bool stop_discovery() override {
    return with_adapter([](sdbus::IProxy& a) {
      a.callMethod("StopDiscovery").onInterface(sdbus::InterfaceName{"org.bluez.Adapter1"});
    });
  }

  void set_sink(std::function<void(const AndroidBtState&)> on_change) override {
    std::lock_guard<std::mutex> lk(mu_);
    sink_ = std::move(on_change);
  }

 private:
  template <class F>
  bool with_adapter(F&& fn) {
    std::string path;
    { std::lock_guard<std::mutex> lk(mu_); path = adapter_path_; }
    if (path.empty()) return false;
    try {
      auto a = sdbus::createProxy(*conn_, sdbus::ServiceName{"org.bluez"}, sdbus::ObjectPath{path});
      fn(*a);
      return true;
    } catch (const sdbus::Error& e) {
      log::w(kTag, "adapter op failed ({})", e.getMessage());
      return false;
    }
  }

  void rebuild() {
    AndroidBtState st;
    std::string adapter_path;
    try {
      ManagedObjects objs;
      om_->callMethod("GetManagedObjects")
          .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.ObjectManager"})
          .storeResultsTo(objs);
      for (const auto& [path, ifaces] : objs) {
        if (auto ad = ifaces.find("org.bluez.Adapter1");
            ad != ifaces.end() && adapter_path.empty()) {
          adapter_path = path;
          const PropMap& p = ad->second;
          st.present = true;
          st.state = get_bool(p, "Powered") ? kStateOn : kStateOff;
          st.address = get_str(p, "Address");
          st.name = get_str(p, "Alias");
          if (st.name.empty()) st.name = get_str(p, "Name");
          st.discovering = get_bool(p, "Discovering");
        }
        if (auto dv = ifaces.find("org.bluez.Device1"); dv != ifaces.end()) {
          const PropMap& p = dv->second;
          BtDevice d;
          d.address = get_str(p, "Address");
          d.name = get_str(p, "Alias");
          if (d.name.empty()) d.name = get_str(p, "Name");
          d.cls = static_cast<int>(get_u32(p, "Class"));
          d.bond_state = get_bool(p, "Paired") ? kBondBonded : kBondNone;
          d.connected = get_bool(p, "Connected");
          st.devices.push_back(std::move(d));  // all devices; JNI filters bonded
        }
      }
    } catch (const sdbus::Error& e) {
      log::w(kTag, "GetManagedObjects failed ({})", e.getMessage());
    }

    bool new_adapter = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      state_ = st;
      if (!adapter_path.empty() && adapter_path != adapter_path_) {
        adapter_path_ = adapter_path;
        new_adapter = true;
      }
    }
    if (new_adapter) {
      // Persistent proxy just for the adapter's PropertiesChanged (Powered/Discovering).
      auto ap = sdbus::createProxy(*conn_, sdbus::ServiceName{"org.bluez"},
                                   sdbus::ObjectPath{adapter_path_});
      ap->uponSignal("PropertiesChanged")
          .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.Properties"})
          .call([this](const std::string&, const PropMap&, const std::vector<std::string>&) {
            rebuild();
          });
      std::lock_guard<std::mutex> lk(mu_);
      adapter_sub_ = std::move(ap);
    }
    notify();
  }

  void notify() {
    std::function<void(const AndroidBtState&)> sink;
    AndroidBtState st;
    {
      std::lock_guard<std::mutex> lk(mu_);
      sink = sink_;
      st = state_;
    }
    if (sink) sink(st);
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IProxy> om_;
  std::unique_ptr<sdbus::IProxy> adapter_sub_;
  mutable std::mutex mu_;
  AndroidBtState state_;
  std::string adapter_path_;
  std::function<void(const AndroidBtState&)> sink_;
};

#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<IBtBackend> make_bt_backend() {
#if ACL_HAVE_SDBUS
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSystemBusConnection();
  } catch (const sdbus::Error& e) {
    log::w(kTag, "no system bus ({}); bluetooth off (null backend)", e.getMessage());
    return std::make_unique<NullBackend>();
  }
  const char* env = std::getenv("ACL_BT_BACKEND");
  const std::string_view pref = env != nullptr ? env : "";
  if ((pref.empty() || pref == "bluez") && name_has_owner(*conn, "org.bluez")) {
    log::i(kTag, "bluetooth backend: bluez");
    return std::make_unique<BlueZBackend>(std::move(conn));
  }
  log::i(kTag, "no BlueZ on the bus; bluetooth off (null backend)");
#endif  // ACL_HAVE_SDBUS
  return std::make_unique<NullBackend>();
}

}  // namespace acl::bt

// SPDX-License-Identifier: Apache-2.0
// Provenance: UDisks2 org.freedesktop.UDisks2 Block/Filesystem/Drive +
// org.freedesktop.DBus. ObjectManager D-Bus API docs →
// android.os.storage.StorageVolume (AOSP framework api-33, Apache-2.0).
// sdbus-c++ + cached-object-model pattern after jwinarske/sdbus-cpp-examples
// and jwinarske/bluez_native (Apache-2.0). No GPL-3.0 sources.
#include "svc/storage/storage_backend.h"

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

namespace acl::storage {
namespace {

constexpr std::string_view kTag = "svc.storage";

// The Null backend: no UDisks2 (or no sdbus). No removable media → empty volume
// list.
class NullBackend : public IStorageBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  AndroidStorageState snapshot() const override { return {}; }
  void set_sink(std::function<void(const AndroidStorageState&)>) override {}
};

#if ACL_HAVE_SDBUS

using PropMap = std::map<std::string, sdbus::Variant>;
using IfaceMap = std::map<std::string, PropMap>;
using ManagedObjects = std::map<sdbus::ObjectPath, IfaceMap>;

bool get_bool(const PropMap& p, const char* k) {
  auto it = p.find(k);
  if (it == p.end())
    return false;
  try {
    return it->second.get<bool>();
  } catch (...) {
    return false;
  }
}
std::string get_str(const PropMap& p, const char* k) {
  auto it = p.find(k);
  if (it == p.end())
    return {};
  try {
    return it->second.get<std::string>();
  } catch (...) {
    return {};
  }
}
std::string get_objpath(const PropMap& p, const char* k) {
  auto it = p.find(k);
  if (it == p.end())
    return {};
  try {
    return it->second.get<sdbus::ObjectPath>();
  } catch (...) {
    return {};
  }
}
// UDisks2 Filesystem.MountPoints is aay (array of NUL-terminated byte arrays).
// Return the first as a string, or empty if unmounted.
std::string first_mount_point(const PropMap& p) {
  auto it = p.find("MountPoints");
  if (it == p.end())
    return {};
  try {
    const auto mps = it->second.get<std::vector<std::vector<std::uint8_t>>>();
    if (mps.empty())
      return {};
    const std::vector<std::uint8_t>& b = mps.front();
    std::size_t n = b.size();
    while (n > 0 && b[n - 1] == 0)
      --n;  // strip trailing NUL(s)
    return std::string(reinterpret_cast<const char*>(b.data()), n);
  } catch (...) {
    return {};
  }
}

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

// UDisks2 backend: snapshot the object tree via ObjectManager.GetManagedObjects
// into a small cached list of mounted removable filesystems, rebuilt on
// InterfacesAdded/Removed. Synchronous Android reads answer off the cache;
// nothing here mutates host storage (read-only scope).
class UDisks2Backend : public IStorageBackend {
 public:
  explicit UDisks2Backend(std::unique_ptr<sdbus::IConnection> conn)
      : conn_(std::move(conn)) {
    om_ = sdbus::createProxy(*conn_,
                             sdbus::ServiceName{"org.freedesktop.UDisks2"},
                             sdbus::ObjectPath{"/org/freedesktop/UDisks2"});
    om_->uponSignal("InterfacesAdded")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.ObjectManager"})
        .call([this](const sdbus::ObjectPath&, const IfaceMap&) { rebuild(); });
    om_->uponSignal("InterfacesRemoved")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.ObjectManager"})
        .call([this](const sdbus::ObjectPath&,
                     const std::vector<std::string>&) { rebuild(); });
    rebuild();
    conn_->enterEventLoopAsync();
  }
  ~UDisks2Backend() override {
    try {
      conn_->leaveEventLoop();
    } catch (...) {
    }
  }

  const char* name() const noexcept override { return "udisks2"; }

  AndroidStorageState snapshot() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
  }

  void set_sink(
      std::function<void(const AndroidStorageState&)> on_change) override {
    std::lock_guard<std::mutex> lk(mu_);
    sink_ = std::move(on_change);
  }

 private:
  // A block object is an Android removable StorageVolume when it (a) is a
  // mounted Filesystem, (b) is not a system/root block or ignored, and (c) its
  // Drive is removable/ejectable.
  static bool drive_is_removable(const ManagedObjects& objs,
                                 const std::string& drive_path) {
    if (drive_path.empty() || drive_path == "/")
      return false;
    auto it = objs.find(sdbus::ObjectPath{drive_path});
    if (it == objs.end())
      return false;
    auto dr = it->second.find("org.freedesktop.UDisks2.Drive");
    if (dr == it->second.end())
      return false;
    const PropMap& p = dr->second;
    if (get_bool(p, "Removable") || get_bool(p, "MediaRemovable") ||
        get_bool(p, "Ejectable"))
      return true;
    const std::string bus = get_str(p, "ConnectionBus");
    return bus == "usb" || bus == "sdio";
  }
  static std::string drive_model(const ManagedObjects& objs,
                                 const std::string& drive_path) {
    auto it = objs.find(sdbus::ObjectPath{drive_path});
    if (it == objs.end())
      return {};
    auto dr = it->second.find("org.freedesktop.UDisks2.Drive");
    if (dr == it->second.end())
      return {};
    return get_str(dr->second, "Model");
  }

  void rebuild() {
    AndroidStorageState st;
    try {
      ManagedObjects objs;
      om_->callMethod("GetManagedObjects")
          .onInterface(
              sdbus::InterfaceName{"org.freedesktop.DBus.ObjectManager"})
          .storeResultsTo(objs);
      for (const auto& [path, ifaces] : objs) {
        auto fs = ifaces.find("org.freedesktop.UDisks2.Filesystem");
        auto blk = ifaces.find("org.freedesktop.UDisks2.Block");
        if (fs == ifaces.end() || blk == ifaces.end())
          continue;
        const std::string mount = first_mount_point(fs->second);
        if (mount.empty())
          continue;  // not mounted → not an active StorageVolume
        const PropMap& b = blk->second;
        if (get_bool(b, "HintSystem") || get_bool(b, "HintIgnore"))
          continue;  // root/system
        if (!drive_is_removable(objs, get_objpath(b, "Drive")))
          continue;  // fixed disk

        StorageVol v;
        v.path = mount;
        v.fs_uuid = get_str(b, "IdUUID");
        v.fs_type = get_str(b, "IdType");
        v.description = get_str(b, "IdLabel");
        if (v.description.empty())
          v.description = drive_model(objs, get_objpath(b, "Drive"));
        if (v.description.empty())
          v.description = "USB storage";
        v.id = "public:" + (v.fs_uuid.empty() ? mount : v.fs_uuid);
        v.state = "mounted";
        st.volumes.push_back(std::move(v));
      }
    } catch (const sdbus::Error& e) {
      log::w(kTag, "GetManagedObjects failed ({})", e.getMessage());
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      state_ = std::move(st);
    }
    notify();
  }

  void notify() {
    std::function<void(const AndroidStorageState&)> sink;
    AndroidStorageState st;
    {
      std::lock_guard<std::mutex> lk(mu_);
      sink = sink_;
      st = state_;
    }
    if (sink)
      sink(st);
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IProxy> om_;
  mutable std::mutex mu_;
  AndroidStorageState state_;
  std::function<void(const AndroidStorageState&)> sink_;
};

#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<IStorageBackend> make_storage_backend() {
#if ACL_HAVE_SDBUS
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSystemBusConnection();
  } catch (const sdbus::Error& e) {
    log::w(kTag, "no system bus ({}); storage off (null backend)",
           e.getMessage());
    return std::make_unique<NullBackend>();
  }
  const char* env = std::getenv("ACL_STORAGE_BACKEND");
  const std::string_view pref = env != nullptr ? env : "";
  if ((pref.empty() || pref == "udisks2") &&
      name_has_owner(*conn, "org.freedesktop.UDisks2")) {
    log::i(kTag, "storage backend: udisks2");
    return std::make_unique<UDisks2Backend>(std::move(conn));
  }
  log::i(kTag, "no UDisks2 on the bus; removable storage off (null backend)");
#endif  // ACL_HAVE_SDBUS
  return std::make_unique<NullBackend>();
}

}  // namespace acl::storage

// SPDX-License-Identifier: Apache-2.0
// Provenance: Android NotificationManager mapped to the freedesktop Desktop
// Notifications spec (org.freedesktop.Notifications); sdbus-cpp usage pattern after
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
#include "svc/notify/notify_backend.h"

#include "acl_config.h"
#include "log/log.h"

#include <mutex>
#include <string_view>

#if ACL_HAVE_SDBUS
#include <map>
#include <vector>
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::notify {
namespace {

constexpr std::string_view kTag = "svc.notify";

// No daemon on the bus → notifications are accepted and dropped.
class NullBackend final : public INotifyBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  uint32_t notify(const std::string&, uint32_t, const std::string&,
                  const std::string&) override { return 0; }
  void close(uint32_t) override {}
  void set_sink(std::function<void(const NotifyEvent&)>) override {}
};

#if ACL_HAVE_SDBUS
constexpr const char* kService = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";
constexpr const char* kIface = "org.freedesktop.Notifications";

class FreedesktopBackend final : public INotifyBackend {
 public:
  explicit FreedesktopBackend(std::unique_ptr<sdbus::IConnection> conn)
      : conn_(std::move(conn)) {
    proxy_ = sdbus::createProxy(*conn_, sdbus::ServiceName{kService},
                                sdbus::ObjectPath{kPath});
    proxy_->uponSignal("ActionInvoked").onInterface(sdbus::InterfaceName{kIface})
        .call([this](const uint32_t id, const std::string& action_key) {
          emit({NotifyEvent::Kind::kActionInvoked, id, action_key, 0});
        });
    proxy_->uponSignal("NotificationClosed").onInterface(sdbus::InterfaceName{kIface})
        .call([this](const uint32_t id, const uint32_t reason) {
          emit({NotifyEvent::Kind::kClosed, id, std::string{}, reason});
        });
    conn_->enterEventLoopAsync();
  }

  const char* name() const noexcept override { return "freedesktop"; }

  uint32_t notify(const std::string& app, uint32_t replaces_id,
                  const std::string& summary, const std::string& body) override {
    std::vector<std::string> actions;               // no actions mapped yet
    std::map<std::string, sdbus::Variant> hints;
    uint32_t id = 0;
    try {
      proxy_->callMethod("Notify").onInterface(sdbus::InterfaceName{kIface})
          .withArguments(app, replaces_id, std::string{} /*icon*/, summary, body,
                         actions, hints, int32_t{-1} /*default timeout*/)
          .storeResultsTo(id);
    } catch (const sdbus::Error& e) {
      log::w(kTag, "Notify failed: {}", e.getMessage());
    }
    return id;
  }

  void close(uint32_t id) override {
    try {
      proxy_->callMethod("CloseNotification").onInterface(sdbus::InterfaceName{kIface})
          .withArguments(id);
    } catch (const sdbus::Error& e) {
      log::w(kTag, "CloseNotification failed: {}", e.getMessage());
    }
  }

  void set_sink(std::function<void(const NotifyEvent&)> on_event) override {
    std::lock_guard<std::mutex> lk(m_);
    sink_ = std::move(on_event);
  }

 private:
  void emit(const NotifyEvent& ev) {
    std::function<void(const NotifyEvent&)> cb;
    {
      std::lock_guard<std::mutex> lk(m_);
      cb = sink_;
    }
    if (cb) cb(ev);
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IProxy> proxy_;
  std::mutex m_;
  std::function<void(const NotifyEvent&)> sink_;
};

bool name_has_owner(sdbus::IConnection& conn, const char* bus_name) {
  try {
    auto dbus = sdbus::createProxy(conn, sdbus::ServiceName{"org.freedesktop.DBus"},
                                   sdbus::ObjectPath{"/org/freedesktop/DBus"});
    bool owned = false;
    dbus->callMethod("NameHasOwner").onInterface(sdbus::InterfaceName{"org.freedesktop.DBus"})
        .withArguments(std::string{bus_name}).storeResultsTo(owned);
    return owned;
  } catch (const sdbus::Error&) {
    return false;
  }
}
#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<INotifyBackend> make_notify_backend() {
#if ACL_HAVE_SDBUS
  // Desktop notifications live on the SESSION bus (unlike svc/net's system bus).
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSessionBusConnection();
  } catch (const sdbus::Error& e) {
    log::w(kTag, "no session bus ({}); notifications dropped", e.getMessage());
    return std::make_unique<NullBackend>();
  }
  if (name_has_owner(*conn, kService)) {
    log::i(kTag, "notification backend: freedesktop (org.freedesktop.Notifications)");
    return std::make_unique<FreedesktopBackend>(std::move(conn));
  }
  log::i(kTag, "no org.freedesktop.Notifications on the session bus; notifications dropped");
#endif
  return std::make_unique<NullBackend>();
}

}  // namespace acl::notify

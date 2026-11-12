// SPDX-License-Identifier: Apache-2.0
// Provenance: Avahi D-Bus API (org.freedesktop.Avahi.Server ServiceBrowserNew /
// ServiceResolverNew / EntryGroupNew; ServiceBrowser ItemNew/ItemRemove;
// ServiceResolver Found/Failure; EntryGroup AddService/Commit) per avahi.org
// docs
// -> android.net.nsd.NsdManager (AOSP android.jar stubs, Apache-2.0). sdbus-cpp
// usage after jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
#include "svc/nsd/nsd_backend.h"

#include "acl_config.h"
#include "log/log.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::nsd {
namespace {

constexpr std::string_view kTag = "svc.nsd";

// Avahi wildcards: any interface, any protocol (IPv4/IPv6).
constexpr std::int32_t kIfUnspec = -1;
constexpr std::int32_t kProtoUnspec = -1;

// No host discovery daemon: NsdManager reports failure rather than hanging a
// listener.
class NullBackend : public INsdBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  bool available() const noexcept override { return false; }
  long start_discovery(const std::string&) override { return 0; }
  void stop_discovery(long) override {}
  bool resolve(const std::string&,
               const std::string&,
               const std::string&) override {
    return false;
  }
  long register_service(const NsdService&) override { return 0; }
  void unregister_service(long) override {}
  void set_sinks(DiscoverySink, ResolveSink) override {}
};

#if ACL_HAVE_SDBUS

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

// Avahi encodes TXT records as an array of byte arrays, each "key=value".
std::vector<std::pair<std::string, std::string>> decode_txt(
    const std::vector<std::vector<std::uint8_t>>& raw) {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(raw.size());
  for (const auto& rec : raw) {
    const std::string s(rec.begin(), rec.end());
    const auto eq = s.find('=');
    if (eq == std::string::npos)
      out.emplace_back(s, std::string{});
    else
      out.emplace_back(s.substr(0, eq), s.substr(eq + 1));
  }
  return out;
}

std::vector<std::vector<std::uint8_t>> encode_txt(
    const std::vector<std::pair<std::string, std::string>>& txt) {
  std::vector<std::vector<std::uint8_t>> out;
  out.reserve(txt.size());
  for (const auto& [k, v] : txt) {
    const std::string s = v.empty() ? k : (k + "=" + v);
    out.emplace_back(s.begin(), s.end());
  }
  return out;
}

// Avahi backend. Each browse owns a ServiceBrowser object; each resolve owns a
// short-lived ServiceResolver; each advertised service owns an EntryGroup. All
// are kept in handle-keyed maps so the Java side can stop exactly one.
class AvahiBackend : public INsdBackend {
 public:
  explicit AvahiBackend(std::unique_ptr<sdbus::IConnection> conn)
      : conn_(std::move(conn)) {
    server_ =
        sdbus::createProxy(*conn_, sdbus::ServiceName{"org.freedesktop.Avahi"},
                           sdbus::ObjectPath{"/"});
    conn_->enterEventLoopAsync();
  }
  ~AvahiBackend() override {
    try {
      conn_->leaveEventLoop();
    } catch (...) {
    }
  }

  const char* name() const noexcept override { return "avahi"; }
  bool available() const noexcept override { return true; }

  long start_discovery(const std::string& type) override {
    const sdbus::InterfaceName srv{"org.freedesktop.Avahi.Server"};
    try {
      sdbus::ObjectPath path;
      server_->callMethod("ServiceBrowserNew")
          .onInterface(srv)
          .withArguments(kIfUnspec, kProtoUnspec, type, std::string{"local"},
                         static_cast<std::uint32_t>(0))
          .storeResultsTo(path);

      auto browser = sdbus::createProxy(
          *conn_, sdbus::ServiceName{"org.freedesktop.Avahi"}, path);
      const long handle = next_handle();
      const sdbus::InterfaceName bi{"org.freedesktop.Avahi.ServiceBrowser"};
      browser->uponSignal("ItemNew").onInterface(bi).call(
          [this, handle](std::int32_t, std::int32_t, const std::string& n,
                         const std::string& t, const std::string& d,
                         std::uint32_t) {
            emit_discovery(handle, DiscoveryEvent::kFound, n, t, d);
          });
      browser->uponSignal("ItemRemove")
          .onInterface(bi)
          .call([this, handle](std::int32_t, std::int32_t, const std::string& n,
                               const std::string& t, const std::string& d,
                               std::uint32_t) {
            emit_discovery(handle, DiscoveryEvent::kLost, n, t, d);
          });
      {
        std::lock_guard<std::mutex> lk(mu_);
        browsers_.emplace(handle, std::move(browser));
      }
      log::i(kTag, "discovery started: {} (handle {})", type, handle);
      return handle;
    } catch (const sdbus::Error& e) {
      log::w(kTag, "ServiceBrowserNew({}) failed: {}", type, e.getMessage());
      return 0;
    }
  }

  void stop_discovery(long handle) override {
    std::unique_ptr<sdbus::IProxy> browser;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (auto it = browsers_.find(handle); it != browsers_.end()) {
        browser = std::move(it->second);
        browsers_.erase(it);
      }
    }
    if (!browser)
      return;
    try {
      browser->callMethod("Free").onInterface(
          sdbus::InterfaceName{"org.freedesktop.Avahi.ServiceBrowser"});
    } catch (const sdbus::Error&) { /* already gone */
    }
    log::i(kTag, "discovery stopped (handle {})", handle);
  }

  bool resolve(const std::string& name,
               const std::string& type,
               const std::string& domain) override {
    const sdbus::InterfaceName srv{"org.freedesktop.Avahi.Server"};
    try {
      sdbus::ObjectPath path;
      server_->callMethod("ServiceResolverNew")
          .onInterface(srv)
          .withArguments(kIfUnspec, kProtoUnspec, name, type,
                         domain.empty() ? std::string{"local"} : domain,
                         kProtoUnspec, static_cast<std::uint32_t>(0))
          .storeResultsTo(path);

      auto resolver =
          std::make_shared<std::unique_ptr<sdbus::IProxy>>(sdbus::createProxy(
              *conn_, sdbus::ServiceName{"org.freedesktop.Avahi"}, path));
      const sdbus::InterfaceName ri{"org.freedesktop.Avahi.ServiceResolver"};
      (*resolver)->uponSignal("Found").onInterface(ri).call(
          [this, resolver](std::int32_t, std::int32_t, const std::string& n,
                           const std::string& t, const std::string& d,
                           const std::string& host, std::int32_t,
                           const std::string& address, std::uint16_t port,
                           const std::vector<std::vector<std::uint8_t>>& txt,
                           std::uint32_t) {
            NsdService s;
            s.name = n;
            s.type = t;
            s.domain = d;
            s.host = host;
            s.address = address;
            s.port = static_cast<int>(port);
            s.txt = decode_txt(txt);
            emit_resolve(true, s);
            retire(resolver);
          });
      (*resolver)->uponSignal("Failure").onInterface(ri).call(
          [this, resolver](const std::string& err) {
            log::w(kTag, "resolve failed: {}", err);
            emit_resolve(false, NsdService{});
            retire(resolver);
          });
      {
        std::lock_guard<std::mutex> lk(mu_);
        resolvers_.push_back(resolver);
      }
      return true;
    } catch (const sdbus::Error& e) {
      log::w(kTag, "ServiceResolverNew({}) failed: {}", name, e.getMessage());
      return false;
    }
  }

  long register_service(const NsdService& svc) override {
    try {
      sdbus::ObjectPath path;
      server_->callMethod("EntryGroupNew")
          .onInterface(sdbus::InterfaceName{"org.freedesktop.Avahi.Server"})
          .storeResultsTo(path);
      auto group = sdbus::createProxy(
          *conn_, sdbus::ServiceName{"org.freedesktop.Avahi"}, path);
      const sdbus::InterfaceName gi{"org.freedesktop.Avahi.EntryGroup"};
      group->callMethod("AddService")
          .onInterface(gi)
          .withArguments(
              kIfUnspec, kProtoUnspec, static_cast<std::uint32_t>(0), svc.name,
              svc.type, svc.domain, std::string{} /* host: let Avahi pick */,
              static_cast<std::uint16_t>(svc.port), encode_txt(svc.txt));
      group->callMethod("Commit").onInterface(gi);
      const long handle = next_handle();
      {
        std::lock_guard<std::mutex> lk(mu_);
        groups_.emplace(handle, std::move(group));
      }
      log::i(kTag, "registered service {} {} port {} (handle {})", svc.name,
             svc.type, svc.port, handle);
      return handle;
    } catch (const sdbus::Error& e) {
      log::w(kTag, "EntryGroup add/commit failed: {}", e.getMessage());
      return 0;
    }
  }

  void unregister_service(long handle) override {
    std::unique_ptr<sdbus::IProxy> group;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (auto it = groups_.find(handle); it != groups_.end()) {
        group = std::move(it->second);
        groups_.erase(it);
      }
    }
    if (!group)
      return;
    try {
      group->callMethod("Free").onInterface(
          sdbus::InterfaceName{"org.freedesktop.Avahi.EntryGroup"});
    } catch (const sdbus::Error&) { /* already gone */
    }
    log::i(kTag, "unregistered service (handle {})", handle);
  }

  void set_sinks(DiscoverySink on_discovery, ResolveSink on_resolve) override {
    std::lock_guard<std::mutex> lk(sink_mu_);
    discovery_sink_ = std::move(on_discovery);
    resolve_sink_ = std::move(on_resolve);
  }

 private:
  long next_handle() { return ++next_; }

  void emit_discovery(long handle,
                      DiscoveryEvent ev,
                      const std::string& n,
                      const std::string& t,
                      const std::string& d) {
    DiscoverySink sink;
    {
      std::lock_guard<std::mutex> lk(sink_mu_);
      sink = discovery_sink_;
    }
    if (!sink)
      return;
    NsdService s;
    s.name = n;
    s.type = t;
    s.domain = d;
    sink(handle, ev, s);
  }

  void emit_resolve(bool ok, const NsdService& s) {
    ResolveSink sink;
    {
      std::lock_guard<std::mutex> lk(sink_mu_);
      sink = resolve_sink_;
    }
    if (sink)
      sink(ok, s);
  }

  // A resolver is one-shot: drop our reference once it has answered. Deferred
  // to keep the proxy alive for the duration of its own signal callback.
  void retire(const std::shared_ptr<std::unique_ptr<sdbus::IProxy>>& r) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = resolvers_.begin(); it != resolvers_.end(); ++it) {
      if (*it == r) {
        resolvers_.erase(it);
        return;
      }
    }
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IProxy> server_;
  std::mutex mu_;
  std::map<long, std::unique_ptr<sdbus::IProxy>> browsers_;
  std::map<long, std::unique_ptr<sdbus::IProxy>> groups_;
  std::vector<std::shared_ptr<std::unique_ptr<sdbus::IProxy>>> resolvers_;
  std::mutex sink_mu_;
  DiscoverySink discovery_sink_;
  ResolveSink resolve_sink_;
  long next_ = 0;
};

#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<INsdBackend> make_nsd_backend() {
#if ACL_HAVE_SDBUS
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSystemBusConnection();
  } catch (const sdbus::Error& e) {
    log::w(kTag,
           "no system bus ({}); service discovery unavailable (null backend)",
           e.getMessage());
    return std::make_unique<NullBackend>();
  }
  const char* env = std::getenv("ACL_NSD_BACKEND");
  const std::string_view pref = env != nullptr ? env : "";
  if ((pref.empty() || pref == "avahi") &&
      name_has_owner(*conn, "org.freedesktop.Avahi")) {
    log::i(kTag, "nsd backend: avahi");
    return std::make_unique<AvahiBackend>(std::move(conn));
  }
  log::i(kTag,
         "no Avahi on the bus; service discovery unavailable (null backend)");
#endif  // ACL_HAVE_SDBUS
  return std::make_unique<NullBackend>();
}

}  // namespace acl::nsd

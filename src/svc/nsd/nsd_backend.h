// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.net.nsd.NsdManager / NsdServiceInfo public
// surface (AOSP android.jar stubs, Apache-2.0); host mapping from the Avahi
// D-Bus API (org.freedesktop.Avahi.Server / .ServiceBrowser / .ServiceResolver
// / .EntryGroup) documented at avahi.org. sdbus-cpp usage pattern after
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
//
// svc/nsd translates the host mDNS/DNS-SD daemon (Avahi) into the slice of
// NsdManager Android apps use: browse a service type, resolve an instance to a
// host/port/TXT, and advertise one of our own. Discovery is inherently
// asynchronous, so the backend pushes events through sinks; the JNI seam
// forwards them to the Java listeners.
#ifndef ACL_SVC_NSD_NSD_BACKEND_H
#define ACL_SVC_NSD_NSD_BACKEND_H

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace acl::nsd {

// One service instance, as browsed or resolved. Host/address/port/txt are
// filled in only by a resolve (a browse reports just the identity triple).
struct NsdService {
  std::string name;     // instance name, e.g. "Living Room TV"
  std::string type;     // service type, e.g. "_googlecast._tcp"
  std::string domain;   // usually "local"
  std::string host;     // resolved host name
  std::string address;  // resolved IP literal
  int port = 0;
  std::vector<std::pair<std::string, std::string>>
      txt;  // TXT key/value records
};

enum class DiscoveryEvent { kFound, kLost };

// A backend owns a proxy to the host discovery daemon.
// Browsers/resolvers/registrations are identified by opaque non-zero handles so
// the Java side can stop exactly one.
class INsdBackend {
 public:
  virtual ~INsdBackend() = default;

  // Human-readable backend id for logs ("avahi", "null").
  virtual const char* name() const noexcept = 0;

  // False when no host discovery daemon is reachable — NsdManager then fails
  // its listeners with FAILURE_INTERNAL_ERROR instead of silently never calling
  // back.
  virtual bool available() const noexcept = 0;

  // Browse `type` (e.g. "_googlecast._tcp"). Events arrive on the discovery
  // sink, tagged with the returned handle. Returns 0 if the browse could not be
  // started.
  virtual long start_discovery(const std::string& type) = 0;
  virtual void stop_discovery(long handle) = 0;

  // Resolve one instance to host/port/TXT; the result arrives on the resolve
  // sink.
  virtual bool resolve(const std::string& name,
                       const std::string& type,
                       const std::string& domain) = 0;

  // Advertise a service. Returns a handle, or 0 on failure.
  virtual long register_service(const NsdService& svc) = 0;
  virtual void unregister_service(long handle) = 0;

  using DiscoverySink =
      std::function<void(long handle, DiscoveryEvent, const NsdService&)>;
  using ResolveSink = std::function<void(bool ok, const NsdService&)>;

  // Arm the event sinks (at most one each; a second call replaces them).
  virtual void set_sinks(DiscoverySink on_discovery,
                         ResolveSink on_resolve) = 0;
};

// Probe the system bus and construct the backend that owns it: Avahi when
// present (ACL_NSD_BACKEND env override honored), else the Null backend (no
// discovery). Never returns null.
std::unique_ptr<INsdBackend> make_nsd_backend();

}  // namespace acl::nsd

#endif  // ACL_SVC_NSD_NSD_BACKEND_H

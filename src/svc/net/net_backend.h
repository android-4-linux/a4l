// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.net.ConnectivityManager/NetworkCapabilities public
// surface (AOSP android.jar stubs, Apache-2.0); host mapping from freedesktop
// systemd-networkd (org.freedesktop.network1), NetworkManager, and net.connman
// D-Bus API docs; sdbus-cpp usage pattern after jwinarske/sdbus-cpp-examples
// (Apache-2.0). No GPL-3.0 sources.
//
// svc/net translates a host connectivity daemon into the small slice of
// ConnectivityManager state Android apps actually read. Every backend
// (systemd-networkd, NetworkManager, ConnMan) reduces its own D-Bus schema to
// one daemon-agnostic AndroidNetState; L1 (the Java ConnectivityManager shim)
// and L2 (the JNI seam) speak only AndroidNetState and never see D-Bus.
#ifndef ACL_SVC_NET_NET_BACKEND_H
#define ACL_SVC_NET_NET_BACKEND_H

#include <functional>
#include <memory>

namespace acl::net {

// The entire Android-visible connectivity truth, independent of the host daemon.
struct AndroidNetState {
  // Transport of the active/default network. NONE ⇒ no active network.
  enum class Transport { kNone, kWifi, kEthernet, kCellular };

  bool connected = false;          // getActiveNetwork() != null
  bool validated = false;          // NET_CAPABILITY_VALIDATED (internet reachable)
  bool metered = false;            // !NET_CAPABILITY_NOT_METERED
  Transport transport = Transport::kNone;

  bool operator==(const AndroidNetState&) const noexcept = default;
};

// A backend owns a D-Bus proxy to one host daemon. snapshot() answers the
// synchronous ConnectivityManager getters; set_sink() arms an async callback that
// fires on every daemon state/property change (→ NetworkCallback in L1).
class INetBackend {
 public:
  virtual ~INetBackend() = default;

  // Human-readable backend id for logs / profiling buckets ("networkd", ...).
  virtual const char* name() const noexcept = 0;

  // Current best-effort connectivity snapshot.
  virtual AndroidNetState snapshot() const = 0;

  // Register the change sink. Called with the new state whenever the daemon
  // reports a relevant change. At most one sink; a second call replaces it.
  virtual void set_sink(std::function<void(const AndroidNetState&)> on_change) = 0;
};

// Probe the system bus and construct the backend that owns it. Order honors the
// ACL_NET_BACKEND env override, then falls back to whichever daemon is present;
// with no daemon (or no sdbus-c++) returns the Null backend (permanently offline,
// today's stub behavior). Never returns null.
std::unique_ptr<INetBackend> make_net_backend();

}  // namespace acl::net

#endif  // ACL_SVC_NET_NET_BACKEND_H

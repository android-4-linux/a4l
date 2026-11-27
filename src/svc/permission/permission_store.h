// SPDX-License-Identifier: Apache-2.0
// Provenance: the XDG permission store D-Bus interface
// (org.freedesktop.impl.portal.PermissionStore, xdg-desktop-portal
// documentation) and its "devices" table as the Device portal uses it; Android
// runtime permissions (AOSP android.jar stubs, Apache-2.0). No GPL-3.0 sources.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acl::permission {

// What the store holds for one app and one device.
enum class Access {
  kUnset,  // the store has no entry for the app
  kYes,
  kNo,
  kAsk,
  kUnavailable,  // no store on the bus, or the call failed
};

// "unset", "yes", "no", "ask" or "unavailable".
const char* name(Access access) noexcept;

// The store's answer for an app is a list of strings; for the devices table it
// is one of "yes", "no" and "ask". An empty list is kUnset, anything else kNo.
Access decode(const std::vector<std::string>& permissions) noexcept;

// True when `device` is an item of the comma-separated `list`, the form of
// ACL_ALLOW_DEVICES. Items are compared whole; spaces around one are ignored.
bool listed(std::string_view list, std::string_view device) noexcept;

// One GetPermission("devices", device, app) call on the session bus. Waits at
// most two seconds. kUnavailable without sdbus-c++.
Access query_store(const std::string& device, const std::string& app) noexcept;

// Whether an app may use a device, with the store's answers kept for a short
// time: a second for an answer, thirty for a store that did not answer. Only
// kYes allows; a4l has no dialog to put kAsk to the user. Thread-safe.
class DeviceAccess {
 public:
  using Clock = std::chrono::steady_clock;
  using Query =
      std::function<Access(const std::string& device, const std::string& app)>;
  using Now = std::function<Clock::time_point()>;

  // `allow` is the ACL_ALLOW_DEVICES value: devices allowed whatever the store
  // says.
  DeviceAccess(std::string allow, Query query, Now now = &Clock::now);

  bool allowed(const std::string& device, const std::string& app);

 private:
  struct Entry {
    Access access;
    Clock::time_point until;
  };

  std::string allow_;
  Query query_;
  Now now_;
  std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, Entry> cache_;
};

// The process's DeviceAccess: ACL_ALLOW_DEVICES and query_store.
bool device_allowed(const std::string& device, const std::string& app);

}  // namespace acl::permission

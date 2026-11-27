// SPDX-License-Identifier: Apache-2.0
// Provenance: the XDG permission store D-Bus interface
// (org.freedesktop.impl.portal.PermissionStore.GetPermission,
// xdg-desktop-portal documentation). sdbus-cpp usage after
// jwinarske/sdbus-cpp-examples (Apache-2.0) and svc/appearance. No GPL-3.0
// sources.
#include "svc/permission/permission_store.h"

#include "acl_config.h"
#include "log/log.h"

#include <cstdlib>

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::permission {
namespace {

constexpr std::string_view kTag = "svc.permission";
constexpr std::chrono::seconds kAnswerLife{1};
constexpr std::chrono::seconds kUnavailableLife{30};

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

}  // namespace

const char* name(Access a) noexcept {
  switch (a) {
    case Access::kUnset:
      return "unset";
    case Access::kYes:
      return "yes";
    case Access::kNo:
      return "no";
    case Access::kAsk:
      return "ask";
    case Access::kUnavailable:
      return "unavailable";
  }
  return "?";
}

Access decode(const std::vector<std::string>& permissions) noexcept {
  if (permissions.empty())
    return Access::kUnset;
  if (permissions.size() == 1) {
    if (permissions[0] == "yes")
      return Access::kYes;
    if (permissions[0] == "ask")
      return Access::kAsk;
  }
  return Access::kNo;
}

bool listed(std::string_view list, std::string_view device) noexcept {
  if (device.empty())
    return false;
  while (!list.empty()) {
    const auto comma = list.find(',');
    if (trim(list.substr(0, comma)) == device)
      return true;
    if (comma == std::string_view::npos)
      break;
    list.remove_prefix(comma + 1);
  }
  return false;
}

#if ACL_HAVE_SDBUS

Access query_store(const std::string& device, const std::string& app) noexcept {
  try {
    auto conn = sdbus::createSessionBusConnection();
    auto store = sdbus::createProxy(
        *conn,
        sdbus::ServiceName{"org.freedesktop.impl.portal.PermissionStore"},
        sdbus::ObjectPath{"/org/freedesktop/impl/portal/PermissionStore"});
    std::vector<std::string> permissions;
    store->callMethod("GetPermission")
        .onInterface(
            sdbus::InterfaceName{"org.freedesktop.impl.portal.PermissionStore"})
        .withTimeout(std::chrono::seconds(2))
        .withArguments(std::string{"devices"}, device, app)
        .storeResultsTo(permissions);
    return decode(permissions);
  } catch (const sdbus::Error& e) {
    // The store answers NotFound for a table or a device nothing was ever set
    // in, which is an app with no entry.
    if (e.getName() == "org.freedesktop.portal.Error.NotFound")
      return Access::kUnset;
    log::i(kTag, "no permission store ({})", std::string{e.getName()});
    return Access::kUnavailable;
  } catch (...) {
    return Access::kUnavailable;
  }
}

#else

Access query_store(const std::string&, const std::string&) noexcept {
  return Access::kUnavailable;
}

#endif

DeviceAccess::DeviceAccess(std::string allow, Query query, Now now)
    : allow_(std::move(allow)),
      query_(std::move(query)),
      now_(std::move(now)) {}

bool DeviceAccess::allowed(const std::string& device, const std::string& app) {
  if (device.empty() || app.empty())
    return false;
  if (listed(allow_, device))
    return true;
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto at = now_();
  auto key = std::make_pair(device, app);
  const auto it = cache_.find(key);
  if (it != cache_.end() && at < it->second.until)
    return it->second.access == Access::kYes;
  // Under the lock: a second thread asking the same question waits for this
  // answer and does not make its own call.
  const Access access = query_(device, app);
  if (it == cache_.end() || it->second.access != access) {
    if (access == Access::kYes) {
      log::i(kTag, "{} may use the {}", app, device);
    } else {
      log::i(kTag,
             "{} may not use the {} (store: {}); to allow it: flatpak "
             "permission-set devices {} {} yes, or ACL_ALLOW_DEVICES={}",
             app, device, name(access), device, app, device);
    }
  }
  const auto life =
      access == Access::kUnavailable ? kUnavailableLife : kAnswerLife;
  cache_.insert_or_assign(std::move(key), Entry{access, at + life});
  return access == Access::kYes;
}

bool device_allowed(const std::string& device, const std::string& app) {
  static DeviceAccess access = [] {
    const char* allow = std::getenv("ACL_ALLOW_DEVICES");
    return DeviceAccess(allow != nullptr ? allow : "", &query_store);
  }();
  return access.allowed(device, app);
}

}  // namespace acl::permission

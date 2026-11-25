// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.app.NotificationManager behavior (AOSP
// android.jar docs, Apache-2.0) mapped to the freedesktop Desktop Notifications
// spec (org.freedesktop.Notifications: Notify/CloseNotification +
// ActionInvoked/ NotificationClosed); sdbus-cpp usage pattern after
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
//
// svc/notify posts an app's notifications to the host notification daemon
// (GNOME Shell, etc.) over the session bus, and delivers the user's
// action/dismissal back.
#ifndef ACL_SVC_NOTIFY_NOTIFY_BACKEND_H
#define ACL_SVC_NOTIFY_NOTIFY_BACKEND_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace acl::notify {

// The largest image edge a backend sends. A large icon can be a full-size
// album cover, and the daemon takes the pixels in the Notify message.
inline constexpr int kMaxImageEdge = 512;

// One notification as the host daemon takes it.
struct NotifyContent {
  std::string app;           // the posting app's label
  uint32_t replaces_id = 0;  // the server id to replace, 0 for a new one
  std::string summary;       // title
  std::string body;          // text
  // (key, label) pairs. The key comes back in ActionInvoked; "default" is the
  // click on the notification itself.
  std::vector<std::pair<std::string, std::string>> actions;
  // RGBA, 8 bits per channel, not premultiplied, image_w * 4 bytes per row.
  // Empty for no image.
  std::vector<uint8_t> image;
  int image_w = 0;
  int image_h = 0;
  // Stays up after an action is invoked (transport controls).
  bool resident = false;
};

// A host-side notification event routed back to the app layer.
struct NotifyEvent {
  enum class Kind { kActionInvoked, kClosed };
  Kind kind;
  uint32_t id = 0;         // the server id returned by notify()
  std::string action_key;  // kActionInvoked (e.g. "default")
  uint32_t reason =
      0;  // kClosed: 1 expired, 2 dismissed, 3 CloseNotification, 4 undefined
};

class INotifyBackend {
 public:
  virtual ~INotifyBackend() = default;
  virtual const char* name() const noexcept = 0;

  // Post a notification, or replace content.replaces_id; returns the server
  // id (0 on failure / Null backend).
  virtual uint32_t notify(const NotifyContent& content) = 0;
  // Dismiss a previously posted notification by server id.
  virtual void close(uint32_t id) = 0;
  // Register the sink for ActionInvoked/NotificationClosed. At most one;
  // replaces.
  virtual void set_sink(std::function<void(const NotifyEvent&)> on_event) = 0;
};

// Probe the session bus for org.freedesktop.Notifications and construct the
// backend; with no daemon (or no sdbus-c++) returns a Null backend
// (notifications dropped). Never returns null.
std::unique_ptr<INotifyBackend> make_notify_backend();

}  // namespace acl::notify

#endif  // ACL_SVC_NOTIFY_NOTIFY_BACKEND_H

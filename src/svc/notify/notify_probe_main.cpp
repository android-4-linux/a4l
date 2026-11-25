// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool. Posts a test notification through the svc/notify
// host backend (org.freedesktop.Notifications). No GPL-3.0 sources.
//
// Run on a desktop to verify the notification bridge: `acl-notify-probe` pops a
// real notification via the session-bus daemon.
#include "svc/notify/notify_backend.h"

#include <cstdio>

int main() {
  auto backend = acl::notify::make_notify_backend();
  std::printf("backend: %s\n", backend->name());
  acl::notify::NotifyContent content;
  content.app = "acl";
  content.summary = "acl notification bridge";
  content.body = "Hello from svc/notify → org.freedesktop.Notifications";
  uint32_t id = backend->notify(content);
  std::printf("posted notification id=%u\n", id);
  return 0;
}

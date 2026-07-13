// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.NotifyBridge (called from the
// INotificationManager translation) to the svc/notify host backend. No GPL-3.0 sources.
#ifndef ACL_SVC_NOTIFY_NOTIFY_JNI_H
#define ACL_SVC_NOTIFY_NOTIFY_JNI_H

#include <jni.h>

namespace acl::notify {

// Create the host notification backend (make_notify_backend), wire its action/close
// sink to NotifyBridge.onEvent, and RegisterNatives on org.acl.internal.NotifyBridge
// (nNotify/nClose). Best-effort. Safe to call once at host-ART boot.
void register_notify_natives(JNIEnv* env);

}  // namespace acl::notify

#endif  // ACL_SVC_NOTIFY_NOTIFY_JNI_H

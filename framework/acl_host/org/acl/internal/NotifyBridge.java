// SPDX-License-Identifier: Apache-2.0
// Provenance: acl notification seam. The INotificationManager translation (see
// HostBootstrap) calls these natives, backed by svc/notify → the host
// org.freedesktop.Notifications daemon. No GPL-3.0 sources.
package org.acl.internal;

// Bridges android.app.NotificationManager (via the seeded INotificationManager) to
// the host notification daemon. Post/replace/close are native; onEvent is invoked
// from the D-Bus signal thread when the user acts on or dismisses a notification.
public final class NotifyBridge {
    private NotifyBridge() {}

    // Post or replace (replacesId != 0) a host notification; returns the server id.
    public static native long nNotify(String app, int replacesId, String summary, String body);
    public static native void nClose(long id);

    // Called from svc/notify (D-Bus signal thread). kind 0 = action invoked,
    // 1 = closed (reason: 1 expired, 2 dismissed, 3 closed-by-call, 4 undefined).
    // Future: map id → the notification's PendingIntent and fire it. Observable now.
    static void onEvent(long id, int kind, String actionKey, int reason) {
        System.out.println("acl notify event: id=" + id
                + (kind == 0 ? " action=" + actionKey : " closed reason=" + reason));
    }
}

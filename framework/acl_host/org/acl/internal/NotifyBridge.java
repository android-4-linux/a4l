// SPDX-License-Identifier: Apache-2.0
// Provenance: acl notification seam. The INotificationManager translation (see
// HostBootstrap) calls post/cancel here, backed by svc/notify → the host
// org.freedesktop.Notifications daemon. No GPL-3.0 sources.
package org.acl.internal;

import android.app.Notification;
import android.app.PendingIntent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.Icon;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

// Bridges android.app.NotificationManager (via the seeded INotificationManager) to the host
// notification daemon. A notification goes out with its title, text, large icon and actions;
// the daemon's ActionInvoked and NotificationClosed come back on the D-Bus signal thread and
// send the notification's PendingIntents on the main thread.
public final class NotifyBridge {
    private NotifyBridge() {}

    // Post or replace (replacesId != 0) a host notification; returns the server id. actions is
    // key, label, key, label. rgba is w*h*4 bytes, not premultiplied, or null.
    static native long nNotify(String app, int replacesId, String summary, String body,
            String[] actions, byte[] rgba, int w, int h, boolean resident);
    static native void nClose(long id);

    // The longest edge of the image sent. The pixels travel in the Notify message; the native
    // side refuses more than 512.
    static final int MAX_IMAGE_EDGE = 256;
    // The key the daemon sends for a click on the notification itself.
    private static final String DEFAULT_ACTION = "default";

    private static final class Posted {
        long server;  // 0 while the notification is held back
        Notification notification;
        // What was last sent, to skip a repost that changes nothing the desktop shows.
        int signature;
        String pkg;
        int appId;
    }

    // True while the app's media session is on the desktop's media controls. A notification
    // that belongs to a media session is then held back: the controls already show the track
    // and its transport, and the desktop would show the app twice. Android does the same
    // with a media notification and its own media controls.
    private static volatile boolean sMediaControls;

    // By the app's notification id, and by the daemon's. Guarded by the class lock: post and
    // cancel come from any app thread, events from the D-Bus thread.
    private static final Map<Integer, Posted> sByAppId = new HashMap<>();
    private static final Map<Long, Posted> sByServer = new HashMap<>();
    private static final Object sPostLock = new Object();

    /** NotificationManager.notify, and a Service's startForeground. */
    static void post(String pkg, int appId, Notification n) {
        if (n == null) return;
        if (sMediaControls && isMedia(n)) {
            hold(pkg, appId, n);
            return;
        }
        final String title = text(n, Notification.EXTRA_TITLE);
        final String body = text(n, Notification.EXTRA_TEXT);
        final List<String> actions = new ArrayList<>();
        if (n.contentIntent != null) {
            actions.add(DEFAULT_ACTION);
            actions.add("");
        }
        if (n.actions != null) {
            for (int i = 0; i < n.actions.length; i++) {
                final Notification.Action a = n.actions[i];
                if (a == null || a.actionIntent == null) continue;
                actions.add("a" + i);
                actions.add(a.title != null ? a.title.toString() : "");
            }
        }
        final int[] size = new int[2];
        final byte[] rgba = imageOf(n, size);
        int signature = Arrays.hashCode(new Object[]{title, body, actions});
        signature = 31 * signature + Arrays.hashCode(rgba);
        // One post at a time, so two for the same id cannot both go out as new. The class lock
        // is not held across nNotify: that call waits for the daemon's reply, and onEvent, on
        // the D-Bus thread, takes the class lock.
        synchronized (sPostLock) {
            final long prev;
            synchronized (NotifyBridge.class) {
                final Posted p = sByAppId.get(appId);
                if (p != null && p.signature == signature) {
                    // Same on the desktop; the intents may be new.
                    p.notification = n;
                    return;
                }
                prev = p != null ? p.server : 0;
            }
            // Transport controls stay up after a click.
            final boolean resident = n.actions != null && n.actions.length > 0;
            final long server = nNotify(appName(pkg), (int) prev, title, body,
                    actions.toArray(new String[0]), rgba, size[0], size[1], resident);
            if (server == 0) return;
            synchronized (NotifyBridge.class) {
                Posted p = sByAppId.get(appId);
                if (p == null) {
                    p = new Posted();
                    sByAppId.put(appId, p);
                } else {
                    sByServer.remove(p.server);
                }
                p.server = server;
                p.notification = n;
                p.signature = signature;
                p.pkg = pkg;
                p.appId = appId;
                sByServer.put(server, p);
            }
        }
    }

    // A notification of a media session: Notification.MediaStyle names its session.
    private static boolean isMedia(Notification n) {
        if (n.extras == null) return false;
        if (n.extras.containsKey(Notification.EXTRA_MEDIA_SESSION)) return true;
        final String template = n.extras.getString(Notification.EXTRA_TEMPLATE);
        return template != null && template.endsWith("MediaStyle");
    }

    // Keeps the notification without showing it, and takes it down if it is showing.
    private static void hold(String pkg, int appId, Notification n) {
        synchronized (sPostLock) {
            final long shown;
            synchronized (NotifyBridge.class) {
                Posted p = sByAppId.get(appId);
                if (p == null) {
                    p = new Posted();
                    sByAppId.put(appId, p);
                }
                shown = p.server;
                if (shown != 0) sByServer.remove(shown);
                p.server = 0;
                p.notification = n;
                p.signature = 0;
                p.pkg = pkg;
                p.appId = appId;
            }
            if (shown != 0) nClose(shown);
        }
    }

    /**
     * The app's media session came onto the desktop's media controls, or left them. Media
     * notifications are taken down when it comes and shown again when it leaves.
     */
    static void mediaControls(boolean on) {
        sMediaControls = on;
        final List<Posted> affected = new ArrayList<>();
        synchronized (NotifyBridge.class) {
            for (Posted p : sByAppId.values()) {
                if (p.notification != null && isMedia(p.notification)
                        && (on ? p.server != 0 : p.server == 0)) {
                    affected.add(p);
                }
            }
        }
        for (Posted p : affected) {
            if (on) hold(p.pkg, p.appId, p.notification);
            else post(p.pkg, p.appId, p.notification);
        }
    }

    /** NotificationManager.cancel, and stopForeground with removal. */
    static void cancel(int appId) {
        final Posted p;
        synchronized (NotifyBridge.class) {
            p = sByAppId.remove(appId);
            if (p != null) sByServer.remove(p.server);
        }
        if (p != null && p.server != 0) nClose(p.server);
    }

    static void cancelAll() {
        final List<Posted> all;
        synchronized (NotifyBridge.class) {
            all = new ArrayList<>(sByAppId.values());
            sByAppId.clear();
            sByServer.clear();
        }
        for (Posted p : all) {
            if (p.server != 0) nClose(p.server);
        }
    }

    // The name the desktop shows over the notification: the app's label, else its package.
    private static String appName(String pkg) {
        final String label = HostBootstrap.appLabel();
        return label != null ? label : pkg;
    }

    private static String text(Notification n, String key) {
        final CharSequence cs = n.extras != null ? n.extras.getCharSequence(key) : null;
        return cs != null ? cs.toString() : "";
    }

    // An icon naming one of the app's own resources is read from the app's Resources: Icon
    // itself asks the package manager for them, which has none to give here.
    private static Drawable drawableOf(Icon icon, android.content.Context ctx) {
        if (icon.getType() == Icon.TYPE_RESOURCE) {
            final String pkg = icon.getResPackage();
            if (pkg == null || pkg.isEmpty() || pkg.equals(ctx.getPackageName())) {
                return ctx.getResources().getDrawable(icon.getResId(), ctx.getTheme());
            }
        }
        return icon.loadDrawable(ctx);
    }

    // The large icon, or the small one, as RGBA bytes no larger than MAX_IMAGE_EDGE on a side.
    // size receives the width and height. Null when there is no icon or it cannot be drawn.
    private static byte[] imageOf(Notification n, int[] size) {
        try {
            final android.content.Context ctx = HostBootstrap.sAppContext;
            Icon icon = n.getLargeIcon();
            if (icon == null) icon = n.getSmallIcon();
            if (icon == null || ctx == null) return null;
            final Drawable d = drawableOf(icon, ctx);
            if (d == null) return null;
            int w = d.getIntrinsicWidth(), h = d.getIntrinsicHeight();
            if (w <= 0 || h <= 0) w = h = 64;  // a drawable with no size of its own
            final float scale = Math.min(1f, (float) MAX_IMAGE_EDGE / Math.max(w, h));
            w = Math.max(1, Math.round(w * scale));
            h = Math.max(1, Math.round(h * scale));
            final Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            d.setBounds(0, 0, w, h);
            d.draw(new Canvas(bmp));
            final int[] argb = new int[w * h];
            bmp.getPixels(argb, 0, w, 0, 0, w, h);
            bmp.recycle();
            final byte[] out = new byte[argb.length * 4];
            for (int i = 0, o = 0; i < argb.length; i++) {
                final int c = argb[i];
                out[o++] = (byte) (c >> 16);
                out[o++] = (byte) (c >> 8);
                out[o++] = (byte) c;
                out[o++] = (byte) (c >>> 24);
            }
            size[0] = w;
            size[1] = h;
            return out;
        } catch (RuntimeException e) {
            // A notification without its picture is better than none.
            System.err.println("acl notify: icon not sent: " + e);
            return null;
        }
    }

    // Called from svc/notify (D-Bus signal thread). kind 0 = action invoked,
    // 1 = closed (reason: 1 expired, 2 dismissed, 3 closed-by-call, 4 undefined).
    static void onEvent(final long id, final int kind, final String actionKey, final int reason) {
        System.out.println("acl notify event: id=" + id
                + (kind == 0 ? " action=" + actionKey : " closed reason=" + reason));
        PendingIntent send = null;
        synchronized (NotifyBridge.class) {
            final Posted p = sByServer.get(id);
            if (p == null) return;
            final Notification n = p.notification;
            if (kind == 0) {
                if (DEFAULT_ACTION.equals(actionKey)) {
                    send = n.contentIntent;
                } else if (actionKey != null && actionKey.startsWith("a") && n.actions != null) {
                    try {
                        final int i = Integer.parseInt(actionKey.substring(1));
                        if (i >= 0 && i < n.actions.length && n.actions[i] != null) {
                            send = n.actions[i].actionIntent;
                        }
                    } catch (NumberFormatException e) {
                        // Not a key this bridge made.
                    }
                }
            } else {
                // Gone from the desktop. A later post with this app id is a new notification.
                sByServer.remove(id);
                sByAppId.values().remove(p);
                if (reason == 2) send = n.deleteIntent;
            }
        }
        if (send == null) return;
        final PendingIntent target = send;
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override public void run() {
                try {
                    target.send();
                } catch (PendingIntent.CanceledException e) {
                    System.err.println("acl notify: intent cancelled: " + e);
                }
            }
        });
    }
}

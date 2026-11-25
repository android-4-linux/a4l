// SPDX-License-Identifier: Apache-2.0
// Provenance: original; uses the public android.media.browse.MediaBrowser API as a car's
// media app does. No GPL-3.0 sources.
package org.acl.internal;

import android.content.ComponentName;
import android.content.Context;
import android.media.browse.MediaBrowser;
import android.media.session.MediaController;
import android.os.Handler;
import android.os.Looper;

import java.util.List;

/**
 * Hosts one of the app's media browser services the way a car's media app does: it binds the
 * service with android.media.browse.MediaBrowser, reads the root and lists its children.
 *
 * <p>The service is reached through the framework's own binder interface, so it does not
 * matter what the service's base class is called. An app built with R8 renames androidx's
 * MediaBrowserServiceCompat, and a lookup of that class by name does not find it.
 *
 * <p>{@link #host} returns when the browser has connected and the root's children have come,
 * or when either has failed or not happened in time. The binding is kept for as long as the
 * process runs.
 */
final class MediaHost {
    private static final long TIMEOUT_MS = 20_000;

    // Kept so the binding is not dropped with the last reference.
    private static MediaBrowser sBrowser;

    private MediaHost() {}

    private static final class Done extends RuntimeException {
        Done() {
            super(null, null, false, false);
        }
    }

    /**
     * Binds serviceClass and browses its root. Returns null when it connected, else the reason
     * it did not. Runs the main looper while it waits; called on the main thread.
     */
    static String host(final String serviceClass) {
        final Context ctx = HostBootstrap.sAppContext;
        if (ctx == null) return "no application context";
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        final Looper looper = Looper.myLooper();
        final String[] failure = {null};
        final boolean[] done = {false};
        // The main looper cannot be quit, so the wait below is left by an exception thrown
        // from the looper's own dispatch.
        final Runnable finish = () -> {
            if (!done[0]) {
                done[0] = true;
                new Handler(looper).post(() -> { throw new Done(); });
            }
        };

        final MediaBrowser.SubscriptionCallback children = new MediaBrowser.SubscriptionCallback() {
            @Override public void onChildrenLoaded(String parentId,
                    List<MediaBrowser.MediaItem> items) {
                System.out.println("acl media: children of '" + parentId + "': " + items.size()
                        + " item(s)");
                for (MediaBrowser.MediaItem item : items) {
                    final CharSequence title = item.getDescription().getTitle();
                    System.out.println("acl media:   " + (item.isBrowsable() ? "[browse] " : "")
                            + (item.isPlayable() ? "[play] " : "") + item.getMediaId()
                            + " '" + (title != null ? title : "") + "'");
                }
                finish.run();
            }
            @Override public void onError(String parentId) {
                System.out.println("acl media: children of '" + parentId + "': error");
                finish.run();
            }
        };
        final MediaBrowser[] browser = {null};
        final MediaBrowser.ConnectionCallback connection = new MediaBrowser.ConnectionCallback() {
            @Override public void onConnected() {
                final MediaBrowser b = browser[0];
                System.out.println("acl media: browse root: " + b.getRoot());
                try {
                    final MediaController c = new MediaController(ctx, b.getSessionToken());
                    System.out.println("acl media: session: playback="
                            + (c.getPlaybackState() != null ? c.getPlaybackState().getState()
                                    : -1));
                } catch (RuntimeException e) {
                    System.out.println("acl media: session: not read (" + e + ")");
                }
                b.subscribe(b.getRoot(), children);
            }
            @Override public void onConnectionFailed() {
                failure[0] = serviceClass + " refused the connection";
                finish.run();
            }
            @Override public void onConnectionSuspended() {
                failure[0] = serviceClass + " dropped the connection";
                finish.run();
            }
        };
        try {
            browser[0] = new MediaBrowser(ctx,
                    new ComponentName(ctx.getPackageName(), serviceClass), connection, null);
            sBrowser = browser[0];
            browser[0].connect();
        } catch (RuntimeException e) {
            return e.toString();
        }
        new Handler(looper).postDelayed(() -> {
            if (!done[0]) {
                if (!browser[0].isConnected()) {
                    failure[0] = serviceClass + " did not connect in " + TIMEOUT_MS + " ms";
                } else {
                    System.out.println("acl media: no children came in " + TIMEOUT_MS + " ms");
                }
                finish.run();
            }
        }, TIMEOUT_MS);
        try {
            Looper.loop();
        } catch (Done d) {
            // The browse finished, or failed, or ran out of time.
        }
        return failure[0];
    }
}

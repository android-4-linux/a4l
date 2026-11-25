// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK public API (android.app.Notification,
// NotificationManager, PendingIntent). No GPL-3.0 sources.
//
// A notification with a large icon and an action, posted to the host notification daemon. The
// test runs a stand-in daemon that invokes the action; this app waits for its PendingIntent to
// arrive, posts the notification again, changes it, and cancels it, and then shows and removes
// a foreground Service's notification. It logs a "NOTIFY:" line
// per step and "NOTIFY: PASS" at the end, then exits 0. A step that fails logs
// "NOTIFY: FAIL <why>" and exits 1; one that never finishes is caught by a 20 s limit (exit 3).
package com.acl.notify;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

public class Main extends Activity {
    static final String TAG = "acl";
    static final String ACTION = "com.acl.notify.ACTION";
    static final int ID = 7;

    static void log(String s) {
        Log.i(TAG, "NOTIFY: " + s);
    }

    static void fail(String why) {
        log("FAIL " + why);
        System.exit(1);
    }

    static Main sMain;

    /** startForeground shows the notification and stopForeground takes it down. */
    public static class Playback extends android.app.Service {
        @Override public android.os.IBinder onBind(Intent i) { return null; }

        @Override public int onStartCommand(Intent i, int flags, int startId) {
            startForeground(8, sMain.build("Foreground"));
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            log("PASS");
            // Leave the close time to reach the daemon.
            new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                @Override public void run() { System.exit(0); }
            }, 500);
            return START_NOT_STICKY;
        }
    }

    private final Handler main = new Handler(Looper.getMainLooper());
    private NotificationManager nm;
    private Bitmap art;

    private PendingIntent intent(String which, int requestCode, int flags) {
        return PendingIntent.getBroadcast(this, requestCode,
                new Intent(ACTION).setPackage(getPackageName()).putExtra("which", which),
                flags | PendingIntent.FLAG_IMMUTABLE);
    }

    private Notification build(String title) {
        return new Notification.Builder(this, "playback")
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setLargeIcon(Icon.createWithBitmap(art))
                .setContentTitle(title)
                .setContentText("Artist")
                .setContentIntent(intent("open", 1, 0))
                .addAction(new Notification.Action.Builder(
                        Icon.createWithResource(this, android.R.drawable.ic_media_pause),
                        "Pause", intent("pause", 2, 0)).build())
                .setStyle(new Notification.MediaStyle().setShowActionsInCompactView(0))
                .build();
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        sMain = this;
        main.postDelayed(new Runnable() {
            @Override public void run() {
                log("FAIL timed out");
                System.exit(3);
            }
        }, 20000);

        // The app's own launch intent names its launcher Activity. A media app builds a
        // notification's content intent from it, and PendingIntent.getActivity throws on null.
        final Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        final String launches = launch != null && launch.getComponent() != null
                ? launch.getComponent().getClassName() : "none";
        log("launch intent " + launches);
        if (!Main.class.getName().equals(launches)) System.exit(1);

        // One PendingIntent per kind, request code and Intent; FLAG_NO_CREATE makes none.
        final PendingIntent first = intent("pause", 2, 0);
        final boolean same = first != null && first.equals(intent("pause", 2, 0));
        final boolean other = first != null && !first.equals(intent("pause", 3, 0));
        final boolean none = intent("pause", 99, PendingIntent.FLAG_NO_CREATE) == null;
        log("pending intent same=" + same + " other=" + other + " no create=" + none);
        if (!same || !other || !none) System.exit(1);

        // Not square, so a width and height swapped on the way would show.
        art = Bitmap.createBitmap(600, 300, Bitmap.Config.ARGB_8888);
        art.eraseColor(0xff336699);

        registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) {
                final String which = i.getStringExtra("which");
                log("action received which=" + which);
                if (!"pause".equals(which)) fail("the wrong PendingIntent was sent");
                // The same notification again: nothing to send. Then a new title, which
                // replaces the first on the desktop, and a cancel, which closes it. The title
                // ends in a character outside the basic plane (a musical note).
                nm.notify(ID, build("Track"));
                nm.notify(ID, build("Next track \uD83C\uDFB5"));
                nm.cancel(ID);
                // Last, a foreground Service's notification.
                startService(new Intent(Main.this, Playback.class));
            }
        }, new IntentFilter(ACTION));

        nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel("playback", "Playback",
                NotificationManager.IMPORTANCE_LOW));
        nm.notify(ID, build("Track"));
        log("posted");
    }
}

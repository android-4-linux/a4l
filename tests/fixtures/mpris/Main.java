// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; public Android API only. No GPL-3.0 sources.
//
// A player with a MediaSession and nothing else, for the MPRIS test. It starts on a track
// "First" by "Band" from "Album", 200 s long, playing at 5 s, with a 64x32 picture as a Bitmap;
// the track after it has a 40x20 picture behind a content URI of the app's own provider. What the
// session's callback is asked to do it does to the session and logs as "MPRIS: <command>":
// pause and play change the playback state, skipToNext moves to a track "Second", and seekTo
// sets the position, and stop releases the session. It runs until it is stopped.
//
// It also posts the session's own notification (MediaStyle, naming the session) before the
// session has a track, and again with the second track, and one plain notification.
package com.acl.mpris;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;

public class Main extends Activity {
    private MediaSession session;
    private long position = 5000;

    private static void log(String s) {
        Log.i("acl", "MPRIS: " + s);
    }

    private void state(int state) {
        session.setPlaybackState(new PlaybackState.Builder()
                .setState(state, position, state == PlaybackState.STATE_PLAYING ? 1f : 0f)
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
                        | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SEEK_TO)
                .build());
    }

    // The first track's picture is a Bitmap. The second's is a content URI of this app's own
    // provider, which is how a media app gives a car its artwork.
    private void track(String title, long durationMs, boolean bitmap) {
        final MediaMetadata.Builder md = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, "Band")
                .putString(MediaMetadata.METADATA_KEY_ALBUM, "Album")
                .putLong(MediaMetadata.METADATA_KEY_DURATION, durationMs);
        if (bitmap) {
            final Bitmap art = Bitmap.createBitmap(64, 32, Bitmap.Config.ARGB_8888);
            art.eraseColor(0xFF336699);
            md.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art);
        } else {
            md.putString(MediaMetadata.METADATA_KEY_ALBUM_ART_URI,
                    "content://com.acl.mpris.art/cover");
        }
        session.setMetadata(md.build());
    }

    /** Serves a 40x20 PNG for any URI. */
    public static class Art extends ContentProvider {
        @Override public boolean onCreate() { return true; }

        @Override public ParcelFileDescriptor openFile(Uri uri, String mode)
                throws FileNotFoundException {
            final File f = new File(getContext().getCacheDir(), "cover.png");
            final Bitmap art = Bitmap.createBitmap(40, 20, Bitmap.Config.ARGB_8888);
            art.eraseColor(0xFF996633);
            try (FileOutputStream os = new FileOutputStream(f)) {
                art.compress(Bitmap.CompressFormat.PNG, 100, os);
            } catch (java.io.IOException e) {
                throw new FileNotFoundException(e.toString());
            }
            log("art read");
            return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
        }

        @Override public String getType(Uri uri) { return "image/png"; }
        @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) {
            return null;
        }
        @Override public Uri insert(Uri u, ContentValues v) { return null; }
        @Override public int delete(Uri u, String s, String[] a) { return 0; }
        @Override public int update(Uri u, ContentValues v, String s, String[] a) { return 0; }
    }

    // The session's own notification, and one that has nothing to do with it. The desktop is
    // to show the first only while the session is not on its media controls.
    private void notifyMedia(String title) {
        getSystemService(NotificationManager.class).notify(7,
                new Notification.Builder(this, "playback")
                        .setSmallIcon(android.R.drawable.ic_media_play)
                        .setContentTitle(title)
                        .setStyle(new Notification.MediaStyle()
                                .setMediaSession(session.getSessionToken()))
                        .build());
    }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getSystemService(NotificationManager.class).createNotificationChannel(
                new NotificationChannel("playback", "Playback",
                        NotificationManager.IMPORTANCE_LOW));
        session = new MediaSession(this, "mpris-fixture");
        // Before the session has anything to show: this one reaches the desktop.
        notifyMedia("Now playing");
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPause() {
                log("pause");
                state(PlaybackState.STATE_PAUSED);
            }
            @Override public void onPlay() {
                log("play");
                state(PlaybackState.STATE_PLAYING);
            }
            @Override public void onSkipToNext() {
                log("next");
                position = 0;
                track("Second", 100000, false);
                state(PlaybackState.STATE_PLAYING);
                notifyMedia("Now playing 2");
            }
            @Override public void onStop() {
                log("stop");
                session.release();
            }
            @Override public void onSeekTo(long pos) {
                log("seek " + pos);
                position = pos;
                state(PlaybackState.STATE_PAUSED);
            }
        }, new Handler(Looper.getMainLooper()));
        track("First", 200000, true);
        state(PlaybackState.STATE_PLAYING);
        session.setActive(true);
        getSystemService(NotificationManager.class).notify(8,
                new Notification.Builder(this, "playback")
                        .setSmallIcon(android.R.drawable.ic_media_play)
                        .setContentTitle("Plain").build());
        log("ready");
    }
}

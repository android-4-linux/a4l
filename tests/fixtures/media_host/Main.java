// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; public Android API only. No GPL-3.0 sources.
//
// A media browser service that is not a MediaBrowserService by class: Wrapped extends Service
// and hands out the binder of a MediaBrowserService it keeps inside, which is how androidx's
// MediaBrowserServiceCompat is built. With R8, that androidx class has another name, so a host
// that looks for the base class by name finds neither. a4l binds it through the framework's
// interface instead (org.acl.internal.MediaHost).
//
// The service logs "MEDIAHOST: onGetRoot ..." with whether the caller it was given is this
// package with this process's uid, which is what a service checks before it answers, and
// "MEDIAHOST: session ..." for its MediaSession: see sessionRoundTrip.
package com.acl.mediahost;

import android.app.Activity;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.media.MediaDescription;
import android.media.browse.MediaBrowser.MediaItem;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.Looper;
import android.os.Bundle;
import android.os.IBinder;
import android.service.media.MediaBrowserService;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

public class Main extends Activity {
    public static class Wrapped extends Service {
        private final Inner inner = new Inner();

        @Override public void onCreate() {
            super.onCreate();
            inner.attach(this);
            inner.onCreate();
        }

        @Override public IBinder onBind(Intent intent) {
            return inner.onBind(intent);
        }
    }

    static class Inner extends MediaBrowserService {
        private MediaSession kept;

        void attach(Context base) {
            attachBaseContext(base);
        }

        @Override public void onCreate() {
            super.onCreate();
            // A browser's connection completes once the service has a session token.
            final MediaSession session = new MediaSession(this, "media-host");
            // Held for the service's life: the framework reaches a session's callback through
            // a weak reference, so a session nothing else holds stops taking commands once
            // it is collected.
            kept = session;
            setSessionToken(session.getSessionToken());
            sessionRoundTrip(session);
        }

        // What the session is given, a MediaController reads back and is told of when it
        // changes; what the controller asks for, the session's callback is given. Logs one
        // "MEDIAHOST: session ..." line once the main looper has delivered all of it.
        private void sessionRoundTrip(final MediaSession session) {
            final Handler main = new Handler(Looper.getMainLooper());
            final StringBuilder commands = new StringBuilder();
            session.setCallback(new MediaSession.Callback() {
                @Override public void onPlay() { commands.append("play "); }
                @Override public void onPause() {
                    commands.append("pause ");
                    Log.i("acl", "MEDIAHOST: asked to pause");
                    session.setPlaybackState(new PlaybackState.Builder()
                            .setState(PlaybackState.STATE_PAUSED, 0, 0f)
                            .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE)
                            .build());
                }
                @Override public void onSkipToNext() { commands.append("next "); }
                @Override public void onSeekTo(long pos) { commands.append("seek" + pos + " "); }
                @Override public void onPlayFromMediaId(String id, Bundle extras) {
                    commands.append("id:" + id + " ");
                    Log.i("acl", "MEDIAHOST: asked to play " + id);
                    session.setMetadata(new MediaMetadata.Builder()
                            .putString(MediaMetadata.METADATA_KEY_TITLE, "Playing " + id)
                            .build());
                    session.setPlaybackState(new PlaybackState.Builder()
                            .setState(PlaybackState.STATE_PLAYING, 0, 1f)
                            .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE)
                            .build());
                }
            }, main);
            session.setMetadata(new MediaMetadata.Builder()
                    .putString(MediaMetadata.METADATA_KEY_TITLE, "Song")
                    .putString(MediaMetadata.METADATA_KEY_ARTIST, "Band")
                    .putLong(MediaMetadata.METADATA_KEY_DURATION, 180000).build());
            session.setPlaybackState(new PlaybackState.Builder()
                    .setState(PlaybackState.STATE_PLAYING, 1500, 1f)
                    .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE).build());
            session.setActive(true);

            final MediaController controller =
                    new MediaController(this, session.getSessionToken());
            final MediaMetadata md = controller.getMetadata();
            final PlaybackState ps = controller.getPlaybackState();
            final boolean read = md != null && ps != null
                    && "Song".equals(md.getString(MediaMetadata.METADATA_KEY_TITLE))
                    && "Band".equals(md.getString(MediaMetadata.METADATA_KEY_ARTIST))
                    && md.getLong(MediaMetadata.METADATA_KEY_DURATION) == 180000
                    && ps.getState() == PlaybackState.STATE_PLAYING && ps.getPosition() == 1500
                    && getPackageName().equals(controller.getPackageName());
            final int[] told = {0};
            controller.registerCallback(new MediaController.Callback() {
                @Override public void onMetadataChanged(MediaMetadata m) {
                    if (m != null && "Next".equals(m.getString(MediaMetadata.METADATA_KEY_TITLE)))
                        told[0] |= 1;
                }
                @Override public void onPlaybackStateChanged(PlaybackState st) {
                    if (st != null && st.getState() == PlaybackState.STATE_PAUSED) told[0] |= 2;
                }
                @Override public void onQueueChanged(java.util.List<MediaSession.QueueItem> q) {
                    if (q != null && q.size() == 2) told[0] |= 4;
                    if (q == null) told[0] |= 8;
                }
            }, main);
            session.setMetadata(new MediaMetadata.Builder()
                    .putString(MediaMetadata.METADATA_KEY_TITLE, "Next").build());
            session.setPlaybackState(new PlaybackState.Builder()
                    .setState(PlaybackState.STATE_PAUSED, 0, 0f).build());
            // The queue: two items in, the same two out and one callback; then none.
            final java.util.List<MediaSession.QueueItem> queue = new ArrayList<>();
            queue.add(new MediaSession.QueueItem(new MediaDescription.Builder()
                    .setMediaId("q-1").setTitle("Queued 1").build(), 11));
            queue.add(new MediaSession.QueueItem(new MediaDescription.Builder()
                    .setMediaId("q-2").setTitle("Queued 2").build(), 12));
            session.setQueue(queue);
            session.setQueueTitle("Up next");
            final java.util.List<MediaSession.QueueItem> back = controller.getQueue();
            final boolean queued = back != null && back.size() == 2
                    && back.get(0).getQueueId() == 11
                    && "q-2".equals(back.get(1).getDescription().getMediaId())
                    && "Queued 2".contentEquals(back.get(1).getDescription().getTitle())
                    && "Up next".contentEquals(controller.getQueueTitle());
            session.setQueue(null);
            final boolean emptied = controller.getQueue() == null;
            final MediaController.TransportControls t = controller.getTransportControls();
            t.play();
            t.pause();
            t.skipToNext();
            t.seekTo(42000);
            t.playFromMediaId("track-1", null);
            final int active = ((MediaSessionManager) getSystemService(MEDIA_SESSION_SERVICE))
                    .getActiveSessions(null).size();
            main.post(new Runnable() {
                @Override public void run() {
                    Log.i("acl", "MEDIAHOST: session read=" + read + " told=" + told[0]
                            + " queued=" + queued + " emptied=" + emptied
                            + " active=" + active + " commands=" + commands.toString().trim());
                }
            });
        }

        @Override public BrowserRoot onGetRoot(String clientPackage, int clientUid, Bundle hints) {
            boolean known = false;
            try {
                known = getPackageName().equals(clientPackage)
                        && clientUid == android.os.Process.myUid()
                        && getPackageManager().getPackageInfo(clientPackage, 0)
                                .applicationInfo.uid == clientUid;
            } catch (android.content.pm.PackageManager.NameNotFoundException e) {
                // known stays false
            }
            Log.i("acl", "MEDIAHOST: onGetRoot client known=" + known);
            return known ? new BrowserRoot("root", null) : null;
        }

        @Override public void onLoadChildren(String parentId, Result<List<MediaItem>> result) {
            final List<MediaItem> items = new ArrayList<>();
            if ("root".equals(parentId)) {
                items.add(new MediaItem(new MediaDescription.Builder().setMediaId("albums")
                        .setTitle("Albums").build(), MediaItem.FLAG_BROWSABLE));
                items.add(new MediaItem(new MediaDescription.Builder().setMediaId("track-1")
                        .setTitle("Track 1").build(), MediaItem.FLAG_PLAYABLE));
            } else if ("albums".equals(parentId)) {
                items.add(new MediaItem(new MediaDescription.Builder().setMediaId("track-2")
                        .setTitle("Track 2").setSubtitle("Band").build(),
                        MediaItem.FLAG_PLAYABLE));
                items.add(new MediaItem(new MediaDescription.Builder().setMediaId("album-9")
                        .setTitle("Album 9").build(), MediaItem.FLAG_BROWSABLE));
            }
            result.sendResult(items);
        }
    }
}

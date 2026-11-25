// SPDX-License-Identifier: Apache-2.0
// Provenance: original; the desktop side is the MPRIS D-Bus Interface Specification 2.2
// (freedesktop.org), in src/svc/mpris. No GPL-3.0 sources.
package org.acl.internal;

import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;

import java.io.File;
import java.io.FileOutputStream;

/**
 * Puts the app's media session on the desktop's media controls. It follows the sessions
 * through {@link MediaSessions.Watcher}, hands the current one's title, artist, album,
 * artwork, position and playback state to src/svc/mpris, and turns the desktop's play, pause,
 * next, previous and seek into the session's own transport commands.
 *
 * <p>The current session is the one that last changed and is not released. An app has one, as
 * a rule. While it is on the bus, {@link NotifyBridge} holds back the session's own
 * notification, so the desktop shows the app's playback once.
 *
 * <p>Artwork that comes as a Bitmap is written to a PNG in the app's cache directory, since
 * MPRIS takes a URL. An http, https or file URI in the metadata is passed as it is, and a
 * content URI is read from the app's own provider and written the same way. The work is done
 * on a thread of its own, so setMetadata does not wait for it.
 */
final class MprisBridge implements MediaSessions.Watcher {
    private static final int ART_EDGE = 512;
    // The values of MprisCommand::Kind.
    private static final int PLAY = 0, PAUSE = 1, PLAY_PAUSE = 2, STOP = 3, NEXT = 4,
            PREVIOUS = 5, SEEK_BY = 6, SEEK_TO = 7;
    private static final int CAN_PLAY = 1, CAN_PAUSE = 2, CAN_NEXT = 4, CAN_PREVIOUS = 8,
            CAN_SEEK = 16;

    private static MprisBridge sInstance;

    private final Handler mWorker;
    private MediaSessions.Session mCurrent;   // worker thread
    private long mTrack;                      // worker thread
    private String mTrackKey = "";            // worker thread
    private Bitmap mArtBitmap;                // worker thread
    private int mArtGeneration;               // worker thread
    private File mArtFile;                    // worker thread
    private String mArtUri;                   // worker thread: the content URI read
    private boolean mOnBus;                   // worker thread
    private boolean mDead;                    // worker thread: no natives here

    private MprisBridge() {
        final HandlerThread t = new HandlerThread("a4l-mpris");
        t.setDaemon(true);
        t.start();
        mWorker = new Handler(t.getLooper());
    }

    /** Starts following the sessions, once. */
    static synchronized void start() {
        if (sInstance != null) return;
        sInstance = new MprisBridge();
        MediaSessions.addWatcher(sInstance);
    }

    @Override public void onSessionChanged(final MediaSessions.Session session) {
        mWorker.post(() -> changed(session));
    }

    private void changed(MediaSessions.Session session) {
        if (mDead) return;
        if (session.destroyed) {
            if (session != mCurrent) return;
            mCurrent = null;
            for (MediaSessions.Session other : MediaSessions.sessions()) {
                if (other.metadata != null || other.playbackState != null) mCurrent = other;
            }
            if (mCurrent == null) {
                leaveBus();
                return;
            }
        } else if (session.metadata != null || session.playbackState != null) {
            mCurrent = session;
        } else {
            return;  // nothing to show yet
        }
        push();
    }

    private void push() {
        final MediaSessions.Session s = mCurrent;
        final MediaMetadata md = s.metadata;
        final PlaybackState ps = s.playbackState;
        final String title = first(md, MediaMetadata.METADATA_KEY_DISPLAY_TITLE,
                MediaMetadata.METADATA_KEY_TITLE);
        final String artist = first(md, MediaMetadata.METADATA_KEY_ARTIST,
                MediaMetadata.METADATA_KEY_ALBUM_ARTIST,
                MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE);
        final String album = first(md, MediaMetadata.METADATA_KEY_ALBUM);
        final long durationMs = md != null ? md.getLong(MediaMetadata.METADATA_KEY_DURATION) : 0;
        final String key = title + '\0' + artist + '\0' + album + '\0' + durationMs + '\0'
                + first(md, MediaMetadata.METADATA_KEY_MEDIA_ID);
        if (!key.equals(mTrackKey)) {
            mTrackKey = key;
            mTrack++;
        }
        boolean onBus;
        try {
            onBus = nUpdate(label(s), s.packageName, playback(ps), title, artist, album,
                    art(md), Math.max(0, durationMs) * 1000L, positionMs(ps) * 1000L,
                    ps != null && ps.getPlaybackSpeed() > 0 ? ps.getPlaybackSpeed() : 1f,
                    can(ps), mTrack);
        } catch (UnsatisfiedLinkError e) {
            mDead = true;  // a build without the session bus backend
            onBus = false;
        }
        if (onBus != mOnBus) {
            mOnBus = onBus;
            // The desktop shows the session once: its media controls, or the notification.
            NotifyBridge.mediaControls(onBus);
        }
    }

    private void leaveBus() {
        if (mOnBus) {
            try {
                nClear();
            } catch (UnsatisfiedLinkError e) {
                mDead = true;
            }
            mOnBus = false;
            NotifyBridge.mediaControls(false);
        }
        if (mArtFile != null) mArtFile.delete();
        mArtFile = null;
        mArtBitmap = null;
        mArtUri = null;
    }

    private static String label(MediaSessions.Session s) {
        final String label = HostBootstrap.appLabel();
        return label != null ? label : s.packageName;
    }

    private static String first(MediaMetadata md, String... keys) {
        if (md == null) return "";
        for (String k : keys) {
            final CharSequence v = md.getText(k);
            if (v != null && v.length() > 0) return v.toString();
        }
        return "";
    }

    // 0 stopped, 1 paused, 2 playing.
    private static int playback(PlaybackState ps) {
        if (ps == null) return 0;
        switch (ps.getState()) {
            case PlaybackState.STATE_PLAYING:
            case PlaybackState.STATE_BUFFERING:
            case PlaybackState.STATE_CONNECTING:
            case PlaybackState.STATE_FAST_FORWARDING:
            case PlaybackState.STATE_REWINDING:
            case PlaybackState.STATE_SKIPPING_TO_NEXT:
            case PlaybackState.STATE_SKIPPING_TO_PREVIOUS:
            case PlaybackState.STATE_SKIPPING_TO_QUEUE_ITEM:
                return 2;
            case PlaybackState.STATE_PAUSED:
                return 1;
            default:
                return 0;
        }
    }

    // Where playback is now: the state's position, moved on by the time since the state was
    // made while it is playing.
    private static long positionMs(PlaybackState ps) {
        if (ps == null) return 0;
        long pos = ps.getPosition();
        if (ps.getState() == PlaybackState.STATE_PLAYING && ps.getLastPositionUpdateTime() > 0) {
            final long since = SystemClock.elapsedRealtime() - ps.getLastPositionUpdateTime();
            if (since > 0) pos += (long) (since * ps.getPlaybackSpeed());
        }
        return Math.max(0, pos);
    }

    private static int can(PlaybackState ps) {
        final long a = ps != null ? ps.getActions() : 0;
        if (a == 0) return CAN_PLAY | CAN_PAUSE;  // a session that names no actions
        int can = 0;
        if ((a & (PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PLAY_PAUSE)) != 0)
            can |= CAN_PLAY;
        if ((a & (PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE)) != 0)
            can |= CAN_PAUSE;
        if ((a & PlaybackState.ACTION_SKIP_TO_NEXT) != 0) can |= CAN_NEXT;
        if ((a & PlaybackState.ACTION_SKIP_TO_PREVIOUS) != 0) can |= CAN_PREVIOUS;
        if ((a & PlaybackState.ACTION_SEEK_TO) != 0) can |= CAN_SEEK;
        return can;
    }

    // The artwork's URL: a PNG of the metadata's Bitmap, written once per Bitmap, else a URI
    // the metadata gives.
    private String art(MediaMetadata md) {
        if (md == null) return "";
        Bitmap bmp = null;
        for (String k : new String[]{MediaMetadata.METADATA_KEY_ALBUM_ART,
                MediaMetadata.METADATA_KEY_ART, MediaMetadata.METADATA_KEY_DISPLAY_ICON}) {
            bmp = md.getBitmap(k);
            if (bmp != null && !bmp.isRecycled()) break;
            bmp = null;
        }
        if (bmp != null) {
            if (bmp == mArtBitmap && bmp.getGenerationId() == mArtGeneration
                    && mArtFile != null) {
                return "file://" + mArtFile.getPath();
            }
            final File written = write(bmp);
            if (written != null) {
                if (mArtFile != null && !mArtFile.equals(written)) mArtFile.delete();
                mArtFile = written;
                mArtBitmap = bmp;
                mArtGeneration = bmp.getGenerationId();
                return "file://" + written.getPath();
            }
        }
        final String uri = first(md, MediaMetadata.METADATA_KEY_ALBUM_ART_URI,
                MediaMetadata.METADATA_KEY_ART_URI, MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI);
        if (uri.startsWith("https://") || uri.startsWith("http://")
                || uri.startsWith("file://")) {
            return uri;
        }
        if (uri.startsWith("content://")) {
            // One of the app's own content providers has the picture, which a media app
            // gives a car this way. It is read once per URI, after the track is shown: the
            // provider may fetch it.
            if (uri.equals(mArtUri)) {
                return mArtFile != null ? "file://" + mArtFile.getPath() : "";
            }
            mArtUri = uri;
            final long track = mTrack;
            mWorker.post(() -> fetch(uri, track));
        }
        return "";
    }

    // Reads the picture at a content URI, writes it as the artwork and shows the track again.
    private void fetch(String uri, long track) {
        final android.content.Context ctx = HostBootstrap.sAppContext;
        if (ctx == null || track != mTrack || !uri.equals(mArtUri)) return;
        Bitmap bmp = null;
        try (java.io.InputStream in =
                ctx.getContentResolver().openInputStream(android.net.Uri.parse(uri))) {
            if (in != null) bmp = android.graphics.BitmapFactory.decodeStream(in);
        } catch (Exception e) {
            android.util.Log.w("acl", "media artwork not read: " + e);
        }
        if (bmp == null) {
            System.out.println("acl media: no artwork from " + uri.substring(0,
                    Math.min(uri.length(), 80)));
            return;
        }
        try {
            final File written = write(bmp);
            if (written == null || track != mTrack || mCurrent == null) {
                if (written != null) written.delete();
                return;
            }
            if (mArtFile != null && !mArtFile.equals(written)) mArtFile.delete();
            mArtFile = written;
            mArtBitmap = null;
            push();
        } finally {
            bmp.recycle();
        }
    }

    // A new name per track, so a desktop that caches by URL shows the new picture.
    private File write(Bitmap bmp) {
        final android.content.Context ctx = HostBootstrap.sAppContext;
        if (ctx == null) return null;
        Bitmap scaled = null;
        File tmp = null;
        try {
            int w = bmp.getWidth(), h = bmp.getHeight();
            if (w <= 0 || h <= 0) return null;
            final float scale = Math.min(1f, (float) ART_EDGE / Math.max(w, h));
            if (scale < 1f) {
                w = Math.max(1, Math.round(w * scale));
                h = Math.max(1, Math.round(h * scale));
                scaled = Bitmap.createScaledBitmap(bmp, w, h, true);
            }
            final File dir = ctx.getCacheDir();
            final File out = new File(dir, "a4l-mpris-" + mTrack + ".png");
            tmp = File.createTempFile(".a4l-mpris", ".tmp", dir);
            try (FileOutputStream os = new FileOutputStream(tmp)) {
                if (!(scaled != null ? scaled : bmp).compress(Bitmap.CompressFormat.PNG, 100,
                        os)) {
                    return null;
                }
            }
            if (!tmp.renameTo(out)) return null;
            tmp = null;
            return out;
        } catch (Exception e) {
            android.util.Log.w("acl", "media artwork not written: " + e);
            return null;
        } finally {
            if (tmp != null) tmp.delete();
            if (scaled != null && scaled != bmp) scaled.recycle();
        }
    }

    /** A command from the desktop, on the bus thread. The session is told on the main one. */
    static void onCommand(final int kind, final long arg) {
        final MprisBridge b;
        synchronized (MprisBridge.class) {
            b = sInstance;
        }
        if (b == null) return;
        b.mWorker.post(() -> {
            final MediaSessions.Session s = b.mCurrent;
            if (s == null) return;
            final PlaybackState ps = s.playbackState;
            new Handler(Looper.getMainLooper()).post(() -> command(s, ps, kind, arg));
        });
    }

    private static void command(MediaSessions.Session s, PlaybackState ps, int kind, long arg) {
        switch (kind) {
            case PLAY:
                s.transport("play");
                break;
            case PAUSE:
                s.transport("pause");
                break;
            case PLAY_PAUSE:
                s.transport(playback(ps) == 2 ? "pause" : "play");
                break;
            case STOP:
                s.transport("stop");
                break;
            case NEXT:
                s.transport("next");
                break;
            case PREVIOUS:
                s.transport("previous");
                break;
            case SEEK_BY:
                s.transport("seekTo", Math.max(0, positionMs(ps) + arg / 1000));
                break;
            case SEEK_TO:
                s.transport("seekTo", Math.max(0, arg / 1000));
                break;
            default:
                break;
        }
    }

    // True when the player is on the session bus.
    private static native boolean nUpdate(String identity, String packageName, int playback,
            String title, String artist, String album, String artUrl, long lengthUs,
            long positionUs, float rate, int can, long track);

    private static native void nClear();
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original; the method names and arguments are those of AOSP's
// android.media.session.ISessionManager, ISession, ISessionCallback, ISessionController and
// ISessionControllerCallback (Apache-2.0), which MediaSession and MediaController call.
// No GPL-3.0 sources.
package org.acl.internal;

import android.content.Intent;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.view.KeyEvent;

import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * The app's media sessions: what the system's media session service keeps for it on Android.
 *
 * <p>A MediaSession's metadata, playback state, queue title, extras and flags are kept here,
 * and a MediaController reads them back and is told when they change. A controller's transport
 * calls (play, pause, next, seekTo, playFromMediaId and the rest) reach the session's
 * callback, which is how the app is told to act. There is one app in the process, so every
 * session and every controller is its own.
 *
 * <p>{@link Watcher} is for the host: a desktop media interface or a browse window follows the
 * sessions through it and drives them through {@link Session#transport}.
 */
final class MediaSessions {
    private MediaSessions() {}

    /** Told, on the thread that made the change, when a session's state changes. */
    interface Watcher {
        void onSessionChanged(Session session);
    }

    private static final List<Session> sSessions = new CopyOnWriteArrayList<>();
    private static final List<Watcher> sWatchers = new CopyOnWriteArrayList<>();

    static void addWatcher(Watcher w) {
        sWatchers.add(w);
    }

    /** The sessions that have not been released, oldest first. */
    static List<Session> sessions() {
        return new ArrayList<>(sSessions);
    }

    /** One MediaSession. Fields are read by the host; the app writes them through ISession. */
    static final class Session {
        final String packageName;
        final String tag;
        final Bundle sessionInfo;
        private final Object callback;  // android.media.session.ISessionCallback
        private final List<Object> controllerCallbacks = new CopyOnWriteArrayList<>();
        final Object session;     // android.media.session.ISession
        final Object controller;  // android.media.session.ISessionController
        volatile MediaMetadata metadata;
        volatile PlaybackState playbackState;
        volatile CharSequence queueTitle;
        volatile Bundle extras;
        volatile android.app.PendingIntent launchIntent;
        volatile int flags;
        volatile int ratingType;
        volatile boolean active;
        volatile boolean destroyed;

        Session(String packageName, Object callback, String tag, Bundle sessionInfo)
                throws ClassNotFoundException {
            this.packageName = packageName;
            this.callback = callback;
            this.tag = tag;
            this.sessionInfo = sessionInfo;
            final Class<?> is = Class.forName("android.media.session.ISession");
            final Class<?> ic = Class.forName("android.media.session.ISessionController");
            controller = Proxy.newProxyInstance(ic.getClassLoader(), new Class<?>[]{ic},
                    (p, m, a) -> fromController(m, a));
            session = Proxy.newProxyInstance(is.getClassLoader(), new Class<?>[]{is},
                    (p, m, a) -> fromSession(m, a));
        }

        /**
         * Sends a transport command to the app, as a controller does: "play", "pause", "next",
         * "seekTo" with a Long, "playFromMediaId" with a String and a Bundle, and so on, by
         * ISessionController's method name. False when the session is gone or has no such
         * command.
         */
        boolean transport(String command, Object... args) {
            if (destroyed || callback == null) return false;
            try {
                return toCallback(command, args != null ? args : new Object[0]);
            } catch (Throwable t) {
                android.util.Log.w("acl", "media session " + command + ": " + t);
                return false;
            }
        }

        // ISession: the app's side.
        private Object fromSession(Method m, Object[] a) throws Throwable {
            switch (m.getName()) {
                case "getController":
                    return controller;
                case "setMetadata":
                    metadata = (MediaMetadata) a[0];
                    tell("onMetadataChanged", MediaMetadata.class, metadata);
                    break;
                case "setPlaybackState":
                    playbackState = (PlaybackState) a[0];
                    tell("onPlaybackStateChanged", PlaybackState.class, playbackState);
                    break;
                case "setQueueTitle":
                    queueTitle = (CharSequence) a[0];
                    tell("onQueueTitleChanged", CharSequence.class, queueTitle);
                    break;
                case "setExtras":
                    extras = (Bundle) a[0];
                    tell("onExtrasChanged", Bundle.class, extras);
                    break;
                case "setFlags":
                    flags = (Integer) a[0];
                    break;
                case "setRatingType":
                    ratingType = (Integer) a[0];
                    break;
                case "setLaunchPendingIntent":
                    launchIntent = (android.app.PendingIntent) a[0];
                    break;
                case "setActive":
                    active = (Boolean) a[0];
                    break;
                case "sendEvent":
                    for (Object cb : controllerCallbacks) {
                        call(cb, "onEvent", new Class<?>[]{String.class, Bundle.class},
                                a[0], a[1]);
                    }
                    return null;
                case "destroySession":
                    destroyed = true;
                    active = false;
                    sSessions.remove(this);
                    for (Object cb : controllerCallbacks) {
                        call(cb, "onSessionDestroyed", new Class<?>[0]);
                    }
                    controllerCallbacks.clear();
                    break;
                default:
                    return defaultOf(m.getReturnType());
            }
            for (Watcher w : sWatchers) {
                try {
                    w.onSessionChanged(this);
                } catch (RuntimeException e) {
                    android.util.Log.w("acl", "media session watcher: " + e);
                }
            }
            return null;
        }

        // ISessionController: a MediaController's side.
        private Object fromController(Method m, Object[] a) throws Throwable {
            switch (m.getName()) {
                case "getMetadata":
                    return metadata;
                case "getPlaybackState":
                    return playbackState;
                case "getQueueTitle":
                    return queueTitle;
                case "getExtras":
                    return extras;
                case "getFlags":
                    return (long) flags;
                case "getRatingType":
                    return ratingType;
                case "getPackageName":
                    return packageName;
                case "getTag":
                    return tag;
                case "getSessionInfo":
                    return sessionInfo != null ? sessionInfo : Bundle.EMPTY;
                case "getLaunchPendingIntent":
                    return launchIntent;
                case "getVolumeAttributes":
                    return HostBootstrap.sessionVolumeAttributes(m.getReturnType());
                case "registerCallback":
                    if (destroyed) {
                        call(a[1], "onSessionDestroyed", new Class<?>[0]);
                    } else if (a[1] != null && !controllerCallbacks.contains(a[1])) {
                        controllerCallbacks.add(a[1]);
                    }
                    return null;
                case "unregisterCallback":
                    controllerCallbacks.remove(a[0]);
                    return null;
                case "sendMediaButton": {
                    if (destroyed || callback == null || a[1] == null) return Boolean.FALSE;
                    final Intent button = new Intent(Intent.ACTION_MEDIA_BUTTON)
                            .putExtra(Intent.EXTRA_KEY_EVENT, (KeyEvent) a[1]);
                    callByName(callback, "onMediaButtonFromController", identity(button));
                    return Boolean.TRUE;
                }
                default:
                    break;
            }
            // The rest are commands for the app: X(packageName, args...) is the callback's
            // onX(packageName, pid, uid, args...).
            if (m.getReturnType() == void.class && a != null && a.length >= 1
                    && m.getParameterTypes()[0] == String.class && !destroyed
                    && callback != null) {
                final Object[] rest = new Object[a.length - 1];
                System.arraycopy(a, 1, rest, 0, rest.length);
                toCallback(m.getName(), rest);
                return null;
            }
            return defaultOf(m.getReturnType());
        }

        private boolean toCallback(String command, Object[] args) throws Throwable {
            final String on;
            switch (command) {
                case "skipToQueueItem":
                    on = "onSkipToTrack";
                    break;
                case "sendCustomAction":
                    on = "onCustomAction";
                    break;
                case "sendCommand":
                    on = "onCommand";
                    break;
                case "adjustVolume":
                case "setVolumeTo":
                    return false;  // the host's volume, not the session's
                default:
                    on = "on" + Character.toUpperCase(command.charAt(0)) + command.substring(1);
                    break;
            }
            return callByName(callback, on, identity(args));
        }

        // (packageName, pid, uid, args...): the caller is the process itself.
        private Object[] identity(Object... args) {
            final Object[] out = new Object[args.length + 3];
            out[0] = packageName;
            out[1] = android.os.Process.myPid();
            out[2] = android.os.Process.myUid();
            System.arraycopy(args, 0, out, 3, args.length);
            return out;
        }

        private void tell(String method, Class<?> type, Object value) {
            for (Object cb : controllerCallbacks) {
                call(cb, method, new Class<?>[]{type}, value);
            }
        }
    }

    // ISessionManager, for the "media_session" service.
    static Object managerCall(Method m, Object[] a) throws Throwable {
        switch (m.getName()) {
            case "createSession": {
                final Session s = new Session((String) a[0], a[1], (String) a[2], (Bundle) a[3]);
                sSessions.add(s);
                return s.session;
            }
            case "getSessions": {
                final List<MediaSession.Token> out = new ArrayList<>();
                for (Session s : sSessions) {
                    if (s.active) out.add(token(s));
                }
                return out;
            }
            case "getMediaKeyEventSession":
                for (Session s : sSessions) {
                    if (s.active) return token(s);
                }
                return null;
            case "getMediaKeyEventSessionPackageName":
                for (Session s : sSessions) {
                    if (s.active) return s.packageName;
                }
                return "";
            default:
                return defaultOf(m.getReturnType());
        }
    }

    private static MediaSession.Token token(Session s) throws Exception {
        final Class<?> ic = Class.forName("android.media.session.ISessionController");
        return MediaSession.Token.class.getConstructor(int.class, ic)
                .newInstance(android.os.Process.myUid(), s.controller);
    }

    // Calls the one method of that name whose argument count fits. The interfaces here have
    // no overloads.
    private static boolean callByName(Object target, String name, Object[] args)
            throws Throwable {
        for (Method m : target.getClass().getMethods()) {
            if (m.getName().equals(name) && m.getParameterCount() == args.length) {
                try {
                    m.invoke(target, args);
                } catch (java.lang.reflect.InvocationTargetException e) {
                    throw e.getCause() != null ? e.getCause() : e;
                }
                return true;
            }
        }
        return false;
    }

    private static void call(Object target, String name, Class<?>[] types, Object... args) {
        if (target == null) return;
        try {
            target.getClass().getMethod(name, types).invoke(target, args);
        } catch (Throwable t) {
            android.util.Log.w("acl", "media session " + name + ": " + t);
        }
    }

    private static Object defaultOf(Class<?> rt) {
        if (rt == boolean.class) return Boolean.FALSE;
        if (rt == int.class) return 0;
        if (rt == long.class) return 0L;
        if (rt == float.class) return 0f;
        return null;
    }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: acl P2.4 display pipeline; Android Looper/Choreographer/MessageQueue/
// DisplayEventReceiver behavior (AOSP docs). ivi-homescreen reactor discipline (Apache-2.0,
// behavioral). No GPL-3.0 sources. See docs/looper-driven-present.md.
package org.acl.internal;

import android.graphics.Canvas;
import android.os.Looper;
import android.os.MessageQueue;
import android.view.Choreographer;
import android.view.View;

import java.io.FileDescriptor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * The Looper-driven present (approach A). Instead of a native poll() loop that only services
 * Wayland — starving the Android main {@link Looper}, so Jetpack Compose's Handler-posted
 * recomposition never runs — this makes the <em>Android Looper own dispatch</em>: the Wayland
 * connection fd is registered with the {@link MessageQueue}, the real {@link Choreographer}
 * drives frames (vsync clocked by wp_presentation on the native side), and the traversal +
 * dma-buf submit run as a {@code CALLBACK_TRAVERSAL} callback — ordered after Compose
 * recomposition (which schedules in the ANIMATION phase), exactly like
 * {@code ViewRootImpl.performTraversals}. Every async framework subsystem is serviced for free.
 *
 * <p>All work runs on the app main thread; {@link #run} blocks in {@link Looper#loop()} until
 * the compositor closes the toplevel.
 */
public final class PresentDriver {
    private PresentDriver() {}

    // Native seam (registered by jvm_vm). The opaque `s` is a native PresentSession* holding
    // the wm presenter, the GPU begin/end-frame entry points, and the input plumbing.
    private static native long nOpen(View decor, int w, int h, String title, String appId);
    private static native int nFd(long s);              // Wayland connection fd
    private static native void nSetImeVisible(long s, boolean visible); // show/hide compositor text-input
    private static native void nSetClipboard(long s, String text);       // put UTF-8 text on the host clipboard

    /** Put UTF-8 {@code text} on the host (Wayland) clipboard, so a Share/copy can be pasted elsewhere. */
    public static void setClipboard(String text) {
        final long s = sSession;
        if (s != 0L && text != null) nSetClipboard(s, text);
    }
    private static native void nSetContentView(long s, View decor);      // swap presented decor (activity transition)

    /** Swap the presented decor to a newly-launched Activity's (internal splash -> main transition). */
    public static boolean setContentView(View decor) {
        final long s = sSession;
        if (s == 0L || decor == null) return false;
        nSetContentView(s, decor);
        return true;
    }

    // Force the whole view tree to re-record (not replay from cache) for the next `frames` present
    // traversals. Used on an activity restore (pop / clear-top): the restored decor is fully laid
    // out but its content RenderNodes hold display lists from before it was backgrounded, which the
    // default cache path never re-traverses -> stale/blank content. A short pulse refreshes it.
    private static native void nPulseForceRecord(long s, int frames);

    /** Re-record the whole tree for the next {@code frames} frames (activity transition refresh). */
    public static void pulseForceRecord(int frames) {
        final long s = sSession;
        if (s != 0L) nPulseForceRecord(s, frames);
    }
    private static native int nRepeatFd(long s);         // wl_keyboard repeat timerfd (-1 if none)
    private static native boolean nDispatch(long s);      // drain wl events; false once closed
    private static native void nDispatchRepeat(long s);   // service one key-repeat tick
    private static native void nSetVsyncDelivered(long s);// switch vsync to compositor-clocked
    private static native void nMaybeDeliverVsync(long s);// if ready + a frame was requested, clock a vsync
    private static native void nTraverse(long s);         // beginFrame -> measure/layout/draw -> submit
    private static native void nClose(long s);            // tear down presenter + release refs

    // Video HW-plane: create a MediaCodec-decodable producer Surface handle (ReaderCtx) for an app's
    // on-screen video SurfaceView, and position the wl_subsurface plane at that view's on-screen rect.
    private static native long nMakeVideoSurface(long s, int w, int h);
    private static native void nSetVideoBounds(long s, int x, int y, int w, int h);

    /** Make a real producer Surface handle (Surface.mNativeObject) for a video SurfaceView; 0 on fail. */
    public static long makeVideoSurface(int w, int h) {
        final long s = sSession;
        return s != 0L ? nMakeVideoSurface(s, w, h) : 0L;
    }

    /** Position the video HW-plane at the video View's on-screen rect (app-surface coords). */
    public static void setVideoBounds(int x, int y, int w, int h) {
        final long s = sSession;
        if (s != 0L) nSetVideoBounds(s, x, y, w, h);
    }

    // GL present (Flutter / SurfaceView): Mesa renders + commits the toplevel; acl owns the Looper
    // and clocks the real Choreographer's vsync from a timerfd (armed with wp_presentation feedback).
    private static native long nGlOpen(View decor, int w, int h, String title, String appId);
    private static native int nGlVsyncFd(long s);         // vsync timerfd (~60Hz), or -1
    private static native void nGlVsync(long s);          // drain timer, arm feedback, tick Choreographer
    private static native void nGlClose(long s);          // tear down GL device + presenter + refs
    // NDK ALooper <-> Java Looper bridge (this thread): Flutter's merged platform+UI task runner
    // registers its frame/microtask timerfd on this thread's NDK ALooper, which acl's Java
    // Looper.loop() does not otherwise service. nGlAloopFd returns that ALooper's epoll fd (readable
    // when a UI task is due); nGlAloopPump drains it. -1 fd if unavailable.
    private static native int nGlAloopFd();
    private static native void nGlAloopPump();

    private static final int CALLBACK_TRAVERSAL = 3;  // Choreographer.CALLBACK_TRAVERSAL (@hide)

    // The current window's native session, for HostBootstrap's InputMethodManager show/hide
    // interception (a focused text field). Set once the presenter opens, cleared on close. The
    // present loop runs on the app's main looper, and InputMethodManager.showSoftInput is called on
    // that same thread, so nSetImeVisible reaches the presenter's text-input on the present thread.
    static volatile long sSession = 0L;

    /** Show/hide the compositor's on-screen keyboard for the current window (no-op if none open). */
    public static void setImeVisible(boolean visible) {
        final long s = sSession;
        if (s != 0L) nSetImeVisible(s, visible);
    }

    /**
     * Present {@code decor} on the current (main) thread. Blocks until the window closes.
     */
    public static void run(View decor, int w, int h, String title, String appId) {
        final long s = nOpen(decor, w, h, title, appId);
        if (s == 0L) {
            android.util.Log.w("acl.present", "PresentDriver: nOpen failed (no compositor?)");
            return;
        }
        sSession = s;  // expose to setImeVisible (InputMethodManager show/hide)
        final Looper looper = Looper.myLooper();
        if (looper == null) { nClose(s); return; }
        final MessageQueue mq = looper.getQueue();
        final Choreographer choreo = Choreographer.getInstance();
        nSetVsyncDelivered(s);  // frames are paced by the compositor's wp_presentation from here

        // Re-posting traversal: runs in the TRAVERSAL phase, after Compose recomposes (ANIMATION).
        final Method postCallback = resolvePostCallback();
        final Runnable[] traversal = new Runnable[1];
        traversal[0] = new Runnable() {
            @Override public void run() {
                // Back a lazily-created video SurfaceView (media3 PlayerView) with a real Surface so
                // ExoPlayer stops rendering to a PlaceholderSurface (which skips every frame) — before
                // the traversal, so the surfaceCreated it fires is reflected this frame.
                HostBootstrap.maybeInjectVideoSurface(decor);
                nTraverse(s);
                postTraversal(choreo, postCallback, traversal[0]);
            }
        };

        // The Wayland fd on the Looper: drain events, then clock a vsync if the compositor is
        // ready and a frame was requested (by Compose's clock or the traversal re-post).
        final MessageQueue.OnFileDescriptorEventListener wlListener =
            new MessageQueue.OnFileDescriptorEventListener() {
                @Override public int onFileDescriptorEvents(FileDescriptor fd, int events) {
                    if (!nDispatch(s)) { quitLooper(looper, s); return 0; }
                    nMaybeDeliverVsync(s);
                    return EVENT_INPUT;
                }
            };
        mq.addOnFileDescriptorEventListener(makeFd(nFd(s)), 0x1 /*EVENT_INPUT*/ | 0x4 /*EVENT_ERROR*/,
                                            wlListener);

        final int repeatFd = nRepeatFd(s);
        if (repeatFd >= 0) {
            mq.addOnFileDescriptorEventListener(makeFd(repeatFd), 0x1 /*EVENT_INPUT*/,
                new MessageQueue.OnFileDescriptorEventListener() {
                    @Override public int onFileDescriptorEvents(FileDescriptor fd, int events) {
                        nDispatchRepeat(s);
                        nMaybeDeliverVsync(s);
                        return EVENT_INPUT;
                    }
                });
        }

        // Bridge the NDK ALooper into this Java Looper (same as runGl). A bionic library on the main
        // thread — notably Chromium WebView's MessagePumpForUI — schedules its tasks by writing an
        // eventfd on the NDK ALooper, which acl reimplements SEPARATELY from the Java MessageQueue's
        // native looper, so Looper.loop() never wakes for those tasks. Without this, WebView main-
        // thread async completions (the ServiceWorker registration lookup, navigation URLLoader
        // callbacks) never run and every navigation hangs at progress=10. Watch the ALooper's epoll
        // fd and drain it when it signals; pump once now to flush anything queued during WebView init.
        final int aloopFd = nGlAloopFd();
        if (aloopFd >= 0) {
            mq.addOnFileDescriptorEventListener(makeFd(aloopFd), 0x1 /*EVENT_INPUT*/ | 0x4 /*EVENT_ERROR*/,
                new MessageQueue.OnFileDescriptorEventListener() {
                    @Override public int onFileDescriptorEvents(FileDescriptor fd, int events) {
                        nGlAloopPump();
                        return EVENT_INPUT;
                    }
                });
            nGlAloopPump();
        }

        // Kick: schedule frame 0 (which requests a vsync) and deliver the first vsync so
        // doFrame — and thus the first composition + traversal — starts.
        postTraversal(choreo, postCallback, traversal[0]);
        nMaybeDeliverVsync(s);

        try {
            Looper.loop();
        } finally {
            sSession = 0L;  // stop routing IME show/hide to a session about to be freed
            nClose(s);
        }
    }

    /**
     * GL present (Flutter / SurfaceView): the app renders GL to the toplevel surface itself (Mesa
     * commits), so there is no View-tree traversal here. acl still owns the main {@link Looper} —
     * Wayland dispatch and the real {@link Choreographer} run on it — and clocks vsync from a
     * timerfd (wp_presentation-aligned on the native side). Ticking the Choreographer is what makes
     * Flutter's VsyncWaiter schedule a frame; Mesa's eglSwapBuffers throttles the actual rendering.
     * Blocks in {@link Looper#loop()} until the compositor closes the toplevel.
     */
    public static void runGl(View decor, int w, int h, String title, String appId) {
        final long s = nGlOpen(decor, w, h, title, appId);
        if (s == 0L) {
            android.util.Log.w("acl.present", "PresentDriver.runGl: nGlOpen failed (no compositor?)");
            return;
        }
        final Looper looper = Looper.myLooper();
        if (looper == null) { nGlClose(s); return; }
        final MessageQueue mq = looper.getQueue();

        // Wayland fd: drain input / xdg / wp_presentation events (false once the toplevel closes).
        mq.addOnFileDescriptorEventListener(makeFd(nFd(s)), 0x1 /*EVENT_INPUT*/ | 0x4 /*EVENT_ERROR*/,
            new MessageQueue.OnFileDescriptorEventListener() {
                @Override public int onFileDescriptorEvents(FileDescriptor fd, int events) {
                    if (!nDispatch(s)) { quitLooper(looper, s); return 0; }
                    return EVENT_INPUT;
                }
            });

        // Vsync timerfd: each tick delivers a vsync to the real Choreographer.
        final int vfd = nGlVsyncFd(s);
        if (vfd >= 0)
            mq.addOnFileDescriptorEventListener(makeFd(vfd), 0x1 /*EVENT_INPUT*/,
                new MessageQueue.OnFileDescriptorEventListener() {
                    @Override public int onFileDescriptorEvents(FileDescriptor fd, int events) {
                        nGlVsync(s);
                        return EVENT_INPUT;
                    }
                });

        // Held-key repeat (if a keyboard is present).
        final int repeatFd = nRepeatFd(s);
        if (repeatFd >= 0)
            mq.addOnFileDescriptorEventListener(makeFd(repeatFd), 0x1 /*EVENT_INPUT*/,
                new MessageQueue.OnFileDescriptorEventListener() {
                    @Override public int onFileDescriptorEvents(FileDescriptor fd, int events) {
                        nDispatchRepeat(s);
                        return EVENT_INPUT;
                    }
                });

        // Bridge this thread's NDK ALooper into the Java Looper. Flutter's merged platform+UI task
        // runner posts its frame/microtask timerfd onto this (app main) thread's NDK ALooper, which
        // Looper.loop() below does not otherwise service — so Dart's warm-up frame never runs. Watch
        // the ALooper's epoll fd and drain it when it signals, then pump once now to flush the tasks
        // (warm-up frame, binding microtasks) already queued during engine startup before we park.
        final int aloopFd = nGlAloopFd();
        if (aloopFd >= 0) {
            mq.addOnFileDescriptorEventListener(makeFd(aloopFd), 0x1 /*EVENT_INPUT*/ | 0x4 /*EVENT_ERROR*/,
                new MessageQueue.OnFileDescriptorEventListener() {
                    @Override public int onFileDescriptorEvents(FileDescriptor fd, int events) {
                        nGlAloopPump();
                        return EVENT_INPUT;
                    }
                });
            nGlAloopPump();
        } else {
            android.util.Log.w("acl.present", "PresentDriver.runGl: NDK ALooper bridge unavailable "
                    + "(fd=-1); Flutter UI tasks will not pump");
        }

        try {
            Looper.loop();
        } finally {
            nGlClose(s);
        }
    }

    private static void postTraversal(Choreographer c, Method postCallback, Runnable r) {
        if (postCallback != null) {
            try { postCallback.invoke(c, CALLBACK_TRAVERSAL, r, null); return; }
            catch (Throwable ignored) { /* fall through to the public API */ }
        }
        // Fallback (no hidden postCallback): a frame callback runs in the ANIMATION phase —
        // same-phase as Compose, so ordering is by post time rather than strictly after.
        c.postFrameCallback(new Choreographer.FrameCallback() {
            @Override public void doFrame(long frameTimeNanos) { r.run(); }
        });
    }

    private static Method resolvePostCallback() {
        try {
            Method m = Choreographer.class.getMethod("postCallback", int.class, Runnable.class,
                                                     Object.class);
            m.setAccessible(true);
            return m;
        } catch (Throwable t) {
            return null;
        }
    }

    // The main looper cannot be quit (it throws); in that case tear down and exit the process,
    // mirroring the old present loop returning and ending the run. A quittable looper unblocks
    // Looper.loop() so the caller's finally runs nClose.
    private static void quitLooper(Looper looper, long s) {
        try {
            looper.quitSafely();
        } catch (Throwable t) {
            nClose(s);
            System.exit(0);
        }
    }

    // MessageQueue.addOnFileDescriptorEventListener takes a java.io.FileDescriptor; wrap the raw
    // integer fd. Android exposes FileDescriptor.setInt$(int); fall back to the private field.
    private static FileDescriptor makeFd(int fd) {
        FileDescriptor jfd = new FileDescriptor();
        try {
            Method setInt = FileDescriptor.class.getDeclaredMethod("setInt$", int.class);
            setInt.setAccessible(true);
            setInt.invoke(jfd, fd);
            return jfd;
        } catch (Throwable ignored) { /* try the field */ }
        for (String name : new String[] { "descriptor", "fd" }) {
            try {
                Field f = FileDescriptor.class.getDeclaredField(name);
                f.setAccessible(true);
                f.setInt(jfd, fd);
                return jfd;
            } catch (Throwable ignored) { /* next candidate */ }
        }
        return jfd;
    }
}

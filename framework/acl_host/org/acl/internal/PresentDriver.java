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
    private static native int nRepeatFd(long s);         // wl_keyboard repeat timerfd (-1 if none)
    private static native boolean nDispatch(long s);      // drain wl events; false once closed
    private static native void nDispatchRepeat(long s);   // service one key-repeat tick
    private static native void nSetVsyncDelivered(long s);// switch vsync to compositor-clocked
    private static native void nMaybeDeliverVsync(long s);// if ready + a frame was requested, clock a vsync
    private static native void nTraverse(long s);         // beginFrame -> measure/layout/draw -> submit
    private static native void nClose(long s);            // tear down presenter + release refs

    private static final int CALLBACK_TRAVERSAL = 3;  // Choreographer.CALLBACK_TRAVERSAL (@hide)

    /**
     * Present {@code decor} on the current (main) thread. Blocks until the window closes.
     */
    public static void run(View decor, int w, int h, String title, String appId) {
        final long s = nOpen(decor, w, h, title, appId);
        if (s == 0L) {
            android.util.Log.w("acl.present", "PresentDriver: nOpen failed (no compositor?)");
            return;
        }
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

        // Kick: schedule frame 0 (which requests a vsync) and deliver the first vsync so
        // doFrame — and thus the first composition + traversal — starts.
        postTraversal(choreo, postCallback, traversal[0]);
        nMaybeDeliverVsync(s);

        try {
            Looper.loop();
        } finally {
            nClose(s);
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

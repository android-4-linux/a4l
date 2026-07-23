// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room acl bridge exposing the host mDNS/DNS-SD daemon (Avahi, via
// the svc/nsd native seam) to android.net.nsd.NsdManager (AOSP android.jar stubs,
// Apache-2.0). No GPL-3.0 sources.
package org.acl.internal;

/**
 * Native seam for service discovery. Browse/resolve results arrive on the host
 * daemon's event-loop thread, so every callback is re-posted to the listener's own
 * looper before it reaches app code — matching NsdManager, which calls listeners back
 * on the thread that registered them.
 *
 * <p>Registered browses/registrations are keyed by the opaque handle the native layer
 * returns, so stopping one discovery never disturbs another.
 */
public final class NsdBridge {
    private NsdBridge() {}

    private static native boolean nAvailable();
    private static native long nStartDiscovery(String type);
    private static native void nStopDiscovery(long handle);
    private static native boolean nResolve(String name, String type, String domain);
    private static native long nRegister(String name, String type, String domain, int port,
                                         String[] txt);
    private static native void nUnregister(long handle);

    /** Receives browse events for one discovery handle, on the caller's looper. */
    public interface DiscoverySink {
        void onFound(String name, String type, String domain);
        void onLost(String name, String type, String domain);
    }

    /** Receives one resolution result, on the caller's looper. */
    public interface ResolveSink {
        void onResolved(boolean ok, String name, String type, String domain, String host,
                        String address, int port, String[] txt);
    }

    private static final java.util.Map<Long, Entry<DiscoverySink>> sDiscovery =
            java.util.Collections.synchronizedMap(new java.util.HashMap<>());
    // Resolves are one-shot and the daemon answers them in order; a queue keeps each
    // pending request paired with the listener that asked for it.
    private static final java.util.Deque<Entry<ResolveSink>> sResolves =
            new java.util.ArrayDeque<>();

    private static final class Entry<T> {
        final T sink;
        final android.os.Handler handler;
        Entry(T sink) {
            this.sink = sink;
            android.os.Looper l = android.os.Looper.myLooper();
            if (l == null) l = android.os.Looper.getMainLooper();
            this.handler = new android.os.Handler(l);
        }
    }

    public static boolean available() {
        try { return nAvailable(); } catch (Throwable t) { return false; }
    }

    /** Begin browsing {@code type}; returns a handle, or 0 when discovery is unavailable. */
    public static long startDiscovery(String type, DiscoverySink sink) {
        try {
            long h = nStartDiscovery(type);
            if (h != 0L && sink != null) sDiscovery.put(h, new Entry<DiscoverySink>(sink));
            return h;
        } catch (Throwable t) {
            return 0L;
        }
    }

    public static void stopDiscovery(long handle) {
        sDiscovery.remove(handle);
        try { nStopDiscovery(handle); } catch (Throwable ignored) {}
    }

    public static boolean resolve(String name, String type, String domain, ResolveSink sink) {
        Entry<ResolveSink> e = sink != null ? new Entry<ResolveSink>(sink) : null;
        synchronized (sResolves) { if (e != null) sResolves.addLast(e); }
        boolean ok;
        try {
            ok = nResolve(name, type, domain);
        } catch (Throwable t) {
            ok = false;
        }
        if (!ok && e != null) synchronized (sResolves) { sResolves.remove(e); }
        return ok;
    }

    public static long registerService(String name, String type, String domain, int port,
                                       String[] txt) {
        try { return nRegister(name, type, domain, port, txt); }
        catch (Throwable t) { return 0L; }
    }

    public static void unregisterService(long handle) {
        try { nUnregister(handle); } catch (Throwable ignored) {}
    }

    // ── called from the host daemon's event thread (svc/nsd) ────────────────────
    static void onServiceFound(final long handle, final String name, final String type,
                               final String domain) {
        final Entry<DiscoverySink> e = sDiscovery.get(handle);
        if (e == null) return;
        e.handler.post(new Runnable() {
            @Override public void run() { e.sink.onFound(name, type, domain); }
        });
    }

    static void onServiceLost(final long handle, final String name, final String type,
                              final String domain) {
        final Entry<DiscoverySink> e = sDiscovery.get(handle);
        if (e == null) return;
        e.handler.post(new Runnable() {
            @Override public void run() { e.sink.onLost(name, type, domain); }
        });
    }

    static void onResolved(final boolean ok, final String name, final String type,
                           final String domain, final String host, final String address,
                           final int port, final String[] txt) {
        final Entry<ResolveSink> e;
        synchronized (sResolves) { e = sResolves.pollFirst(); }
        if (e == null) return;
        e.handler.post(new Runnable() {
            @Override public void run() {
                e.sink.onResolved(ok, name, type, domain, host, address, port, txt);
            }
        });
    }
}

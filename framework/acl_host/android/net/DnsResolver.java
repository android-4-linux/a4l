// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.DnsResolver; the public type surface is the one AOSP's
// API signature file lists (API 29+, Apache-2.0). The real class sends queries to the
// platform resolver through netd; a4l has one network and answers from the host's resolver
// (java.net.InetAddress). No AOSP implementation code. No GPL-3.0 sources.
package android.net;

import android.os.CancellationSignal;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;

// a4l connectivity shim (see Network.java for why android.net.* is here). An app's own HTTP
// stack resolves names with this instead of InetAddress when it wants the answer on an
// executor (Waze's does, once its server configuration turns that path on); without the class
// it dies with NoClassDefFoundError.
//
// query() resolves on a thread of its own and hands the answer to the caller's executor.
// The network argument is ignored: there is one. A name that does not resolve is an empty
// answer with rcode NXDOMAIN, which is what the platform gives; the host resolver does not
// say whether it was that or a failure to reach a server. rawQuery(), which wants the DNS
// packet itself, is not answered: its callback gets ERROR_SYSTEM.
public final class DnsResolver {
    public static final int CLASS_IN = 1;
    public static final int TYPE_A = 1;
    public static final int TYPE_AAAA = 28;

    public static final int FLAG_EMPTY = 0;
    public static final int FLAG_NO_RETRY = 1;
    public static final int FLAG_NO_CACHE_STORE = 2;
    public static final int FLAG_NO_CACHE_LOOKUP = 4;

    public static final int ERROR_PARSE = 0;
    public static final int ERROR_SYSTEM = 1;

    private static final int RCODE_NOERROR = 0;
    private static final int RCODE_NXDOMAIN = 3;
    private static final int TYPE_ANY_ADDRESS = -1;

    private static final DnsResolver sInstance = new DnsResolver();
    private static final AtomicInteger sThreads = new AtomicInteger();

    private DnsResolver() {}

    public static DnsResolver getInstance() {
        return sInstance;
    }

    public interface Callback<T> {
        void onAnswer(T answer, int rcode);
        void onError(DnsException error);
    }

    public static class DnsException extends Exception {
        public final int code;

        public DnsException(int code, Throwable cause) {
            super(cause);
            this.code = code;
        }
    }

    public void query(Network network, String domain, int flags, Executor executor,
            CancellationSignal cancellationSignal,
            Callback<? super List<InetAddress>> callback) {
        resolve(domain, TYPE_ANY_ADDRESS, executor, cancellationSignal, callback);
    }

    public void query(Network network, String domain, int nsType, int flags, Executor executor,
            CancellationSignal cancellationSignal,
            Callback<? super List<InetAddress>> callback) {
        if (nsType != TYPE_A && nsType != TYPE_AAAA) {
            throw new IllegalArgumentException("nsType " + nsType + " is not TYPE_A or TYPE_AAAA");
        }
        resolve(domain, nsType, executor, cancellationSignal, callback);
    }

    public void rawQuery(Network network, byte[] query, int flags, Executor executor,
            CancellationSignal cancellationSignal, Callback<? super byte[]> callback) {
        unanswered(executor, cancellationSignal, callback);
    }

    public void rawQuery(Network network, String domain, int nsClass, int nsType, int flags,
            Executor executor, CancellationSignal cancellationSignal,
            Callback<? super byte[]> callback) {
        unanswered(executor, cancellationSignal, callback);
    }

    private static boolean canceled(CancellationSignal signal) {
        return signal != null && signal.isCanceled();
    }

    private static void resolve(final String domain, final int nsType, final Executor executor,
            final CancellationSignal signal,
            final Callback<? super List<InetAddress>> callback) {
        if (domain == null || executor == null || callback == null) {
            throw new NullPointerException("domain, executor and callback are required");
        }
        if (canceled(signal)) return;
        final Thread t = new Thread(new Runnable() {
            @Override public void run() {
                final List<InetAddress> answer = new ArrayList<InetAddress>();
                try {
                    for (InetAddress a : InetAddress.getAllByName(domain)) {
                        if (nsType == TYPE_A && !(a instanceof Inet4Address)) continue;
                        if (nsType == TYPE_AAAA && !(a instanceof Inet6Address)) continue;
                        answer.add(a);
                    }
                } catch (UnknownHostException e) {
                    answer.clear();
                } catch (final RuntimeException e) {
                    // The resolver itself failed (a SecurityException, say): an error, not
                    // an answer.
                    deliver(executor, signal, new Runnable() {
                        @Override public void run() {
                            callback.onError(new DnsException(ERROR_SYSTEM, e));
                        }
                    });
                    return;
                }
                final int rcode = answer.isEmpty() ? RCODE_NXDOMAIN : RCODE_NOERROR;
                deliver(executor, signal, new Runnable() {
                    @Override public void run() {
                        callback.onAnswer(Collections.unmodifiableList(answer), rcode);
                    }
                });
            }
        }, "a4l-dns-" + sThreads.incrementAndGet());
        t.setDaemon(true);
        t.start();
    }

    private static void unanswered(Executor executor, CancellationSignal signal,
            final Callback<? super byte[]> callback) {
        if (executor == null || callback == null) {
            throw new NullPointerException("executor and callback are required");
        }
        deliver(executor, signal, new Runnable() {
            @Override public void run() {
                callback.onError(new DnsException(ERROR_SYSTEM,
                        new UnsupportedOperationException("a4l answers no raw DNS queries")));
            }
        });
    }

    // A query that was canceled gets no callback. An executor that refuses the work (shut
    // down under the query) leaves it undelivered, as it would on the platform.
    private static void deliver(Executor executor, CancellationSignal signal, Runnable r) {
        if (canceled(signal)) return;
        try {
            executor.execute(r);
        } catch (RuntimeException e) {
            android.util.Log.w("acl", "DnsResolver: the executor refused the answer: " + e);
        }
    }
}

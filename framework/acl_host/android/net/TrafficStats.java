// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.TrafficStats; public static-method surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Device-total byte counters map to the Linux
// /proc/net/dev interface statistics (the real host counters, which is exactly what
// TrafficStats' "since device boot" totals mean); per-UID accounting and socket/thread
// tagging have no host equivalent, so they report UNSUPPORTED / no-op. Not on acl's framework
// runtime boot classpath (it lives in the connectivity apex acl doesn't load), so a shim is
// needed — e.g. Chromium's org.chromium.net.AndroidTrafficStats hard-references it and a
// NoClassDefFoundError there aborts the process via chromium's JNI CheckException. No GPL-3.0 sources.
package android.net;

import java.io.BufferedReader;
import java.io.FileReader;

public class TrafficStats {
    /** Returned when a value is unavailable (e.g. per-UID stats on the host). */
    public static final int UNSUPPORTED = -1;

    private static final ThreadLocal<Integer> sThreadTag = new ThreadLocal<>();
    private static final ThreadLocal<Integer> sThreadUid = new ThreadLocal<>();

    private TrafficStats() {}

    // Sum column `col` (0-based within a /proc/net/dev data row: 0=rx_bytes, 8=tx_bytes) across
    // every non-loopback interface. Returns UNSUPPORTED if the file can't be read.
    private static long sumProcNetDev(int col) {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/net/dev"))) {
            long total = 0;
            String line;
            while ((line = r.readLine()) != null) {
                int colon = line.indexOf(':');
                if (colon < 0) continue;                 // header rows have no ':'
                String iface = line.substring(0, colon).trim();
                if (iface.equals("lo")) continue;        // exclude loopback, like the framework
                String[] f = line.substring(colon + 1).trim().split("\\s+");
                if (col < f.length) total += Long.parseLong(f[col]);
            }
            return total;
        } catch (Throwable t) {
            return UNSUPPORTED;
        }
    }

    public static long getTotalRxBytes() { return sumProcNetDev(0); }   // rx_bytes
    public static long getTotalTxBytes() { return sumProcNetDev(8); }   // tx_bytes
    public static long getTotalRxPackets() { return sumProcNetDev(1); }
    public static long getTotalTxPackets() { return sumProcNetDev(9); }

    public static long getMobileRxBytes() { return getTotalRxBytes(); }
    public static long getMobileTxBytes() { return getTotalTxBytes(); }

    // Per-UID accounting is a kernel xt_qtaguid / eBPF feature Android provides; the host has no
    // per-app split, so report unavailable (callers fall back gracefully).
    public static long getUidRxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidTxBytes(int uid) { return UNSUPPORTED; }
    public static long getUidRxPackets(int uid) { return UNSUPPORTED; }
    public static long getUidTxPackets(int uid) { return UNSUPPORTED; }

    // Socket/thread tagging drives Android's per-tag data accounting; no host equivalent, so the
    // tag/uid are just tracked per-thread for get/set parity and sockets are left untouched.
    public static void setThreadStatsTag(int tag) { sThreadTag.set(tag); }
    public static int getThreadStatsTag() { Integer t = sThreadTag.get(); return t != null ? t : -1; }
    public static void clearThreadStatsTag() { sThreadTag.remove(); }
    public static void setThreadStatsUid(int uid) { sThreadUid.set(uid); }
    public static int getThreadStatsUid() { Integer u = sThreadUid.get(); return u != null ? u : -1; }
    public static void clearThreadStatsUid() { sThreadUid.remove(); }

    public static void tagSocket(java.net.Socket socket) {}
    public static void untagSocket(java.net.Socket socket) {}
    public static void tagDatagramSocket(java.net.DatagramSocket socket) {}
    public static void untagDatagramSocket(java.net.DatagramSocket socket) {}
    public static void incrementOperationCount(int operationCount) {}
    public static void incrementOperationCount(int tag, int operationCount) {}
}

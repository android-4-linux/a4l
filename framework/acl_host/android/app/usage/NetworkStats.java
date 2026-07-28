// SPDX-License-Identifier: Apache-2.0
// Provenance: android.app.usage.NetworkStats + NetworkStats.Bucket public surface (AOSP android.jar
// api-33 stub; Android SDK docs). Like android.app.usage.NetworkStatsManager, the real class is in
// the connectivity apex (absent from acl's bcp), so acl provides a clean-room boot-classpath shim.
// acl has no traffic accounting, so a query yields an EMPTY result set (no buckets) rather than null,
// so a caller's iteration/aggregation loop terminates cleanly instead of NPEing. No GPL-3.0 sources.
package android.app.usage;

/**
 * Shim of {@link android.app.usage.NetworkStats}. Always empty — {@link #hasNextBucket()} is false —
 * because acl exposes no network-usage accounting. Returned by acl's NetworkStatsManager shim.
 */
public class NetworkStats implements AutoCloseable {
    public NetworkStats() {}

    public boolean hasNextBucket() { return false; }
    public boolean getNextBucket(Bucket bucketOut) { return false; }
    @Override public void close() {}

    /** A single usage bucket; acl only ever hands back a zeroed one. */
    public static class Bucket {
        public static final int STATE_ALL = -1;
        public static final int STATE_DEFAULT = 1;
        public static final int STATE_FOREGROUND = 2;

        public static final int UID_ALL = -1;
        public static final int UID_REMOVED = -4;
        public static final int UID_TETHERING = -5;

        public static final int TAG_NONE = 0;

        public static final int METERED_ALL = -1;
        public static final int METERED_NO = 1;
        public static final int METERED_YES = 2;

        public static final int ROAMING_ALL = -1;
        public static final int ROAMING_NO = 1;
        public static final int ROAMING_YES = 2;

        public static final int DEFAULT_NETWORK_ALL = -1;
        public static final int DEFAULT_NETWORK_NO = 1;
        public static final int DEFAULT_NETWORK_YES = 2;

        public Bucket() {}

        public int getUid() { return UID_ALL; }
        public int getTag() { return TAG_NONE; }
        public int getState() { return STATE_ALL; }
        public int getMetered() { return METERED_ALL; }
        public int getRoaming() { return ROAMING_ALL; }
        public int getDefaultNetworkStatus() { return DEFAULT_NETWORK_ALL; }
        public long getStartTimeStamp() { return 0L; }
        public long getEndTimeStamp() { return 0L; }
        public long getRxBytes() { return 0L; }
        public long getTxBytes() { return 0L; }
        public long getRxPackets() { return 0L; }
        public long getTxPackets() { return 0L; }
    }
}

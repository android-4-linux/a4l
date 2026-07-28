// SPDX-License-Identifier: Apache-2.0
// Provenance: android.app.usage.NetworkStatsManager public surface (AOSP android.jar api-33 stub;
// Android SDK docs). NetworkStatsManager lives in the com.android.tethering / framework-connectivity
// apex, absent from acl's boot classpath, so — like android.bluetooth.BluetoothAdapter — acl
// provides a clean-room boot-classpath shim. acl has no per-uid network-usage accounting backend, so
// queries return no data; this exists so an app that resolves getSystemService(NETWORK_STATS_SERVICE)
// and holds the manager (e.g. Sonos's telemetry NetworkWindowLogger) does not NPE on a null service.
// No GPL-3.0 sources.
package android.app.usage;

/**
 * Shim of {@link android.app.usage.NetworkStatsManager}. Returned by acl's Context
 * .getSystemService("netstats"). Usage queries return null / usage callbacks are accepted no-ops —
 * acl exposes no network-statistics service. Callers that merely obtain and retain the manager work;
 * callers that read stats get nothing (acl has no traffic accounting).
 */
public class NetworkStatsManager {
    public NetworkStatsManager() {}

    // Queries return an EMPTY-but-non-null result (a zeroed Bucket / a NetworkStats with no buckets)
    // so a caller's read/iterate/aggregate path runs cleanly; acl has no traffic accounting to report.
    public NetworkStats.Bucket querySummaryForDevice(int networkType, String subscriberId,
            long startTime, long endTime) { return new NetworkStats.Bucket(); }
    public NetworkStats.Bucket querySummaryForUser(int networkType, String subscriberId,
            long startTime, long endTime) { return new NetworkStats.Bucket(); }
    public NetworkStats querySummary(int networkType, String subscriberId,
            long startTime, long endTime) { return new NetworkStats(); }
    public NetworkStats queryDetails(int networkType, String subscriberId,
            long startTime, long endTime) { return new NetworkStats(); }
    public NetworkStats queryDetailsForUid(int networkType, String subscriberId,
            long startTime, long endTime, int uid) { return new NetworkStats(); }
    public NetworkStats queryDetailsForUidTag(int networkType, String subscriberId,
            long startTime, long endTime, int uid, int tag) { return new NetworkStats(); }
    public NetworkStats queryDetailsForUidTagState(int networkType, String subscriberId,
            long startTime, long endTime, int uid, int tag, int state) { return new NetworkStats(); }

    public void registerUsageCallback(int networkType, String subscriberId, long thresholdBytes,
            UsageCallback callback) {}
    public void registerUsageCallback(int networkType, String subscriberId, long thresholdBytes,
            UsageCallback callback, android.os.Handler handler) {}
    public void unregisterUsageCallback(UsageCallback callback) {}

    /** Threshold callback; acl never fires it (no usage accounting). */
    public abstract static class UsageCallback {
        public abstract void onThresholdReached(int networkType, String subscriberId);
    }
}

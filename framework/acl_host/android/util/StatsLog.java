// SPDX-License-Identifier: Apache-2.0
// Provenance: android.util.StatsLog public/@SystemApi surface (AOSP SDK docs + the class's public
// method signatures, Apache-2.0). Clean-room boot-classpath shim: StatsLog lives in the statsd
// apex (framework-statsd), absent on this host, so framework-minus-apex's FrameworkStatsLog.write()
// NoClassDefFoundErrors when a UI interaction logs a metric. acl does no statsd telemetry — every
// entry point discards. Pairs with the [[StatsEvent]] shim. No GPL-3.0 sources.
package android.util;

/** No-op statsd sink: acl records no telemetry, so every log/write is dropped. */
public final class StatsLog {
    public static final byte ANNOTATION_ID_IS_UID = 1;
    public static final byte ANNOTATION_ID_TRUNCATE_TIMESTAMP = 2;
    public static final byte ANNOTATION_ID_PRIMARY_FIELD = 3;
    public static final byte ANNOTATION_ID_EXCLUSIVE_STATE = 4;
    public static final byte ANNOTATION_ID_PRIMARY_FIELD_FIRST_UID = 5;
    public static final byte ANNOTATION_ID_DEFAULT_STATE = 6;
    public static final byte ANNOTATION_ID_TRIGGER_STATE_RESET = 7;
    public static final byte ANNOTATION_ID_STATE_NESTED = 8;

    private StatsLog() { }

    public static boolean logStart(int label) { return false; }
    public static boolean logStop(int label) { return false; }
    public static boolean logEvent(int label) { return false; }
    public static boolean logBinaryPushStateChanged(String trainName, long trainVersionCode,
            int options, int state, long[] experimentIds) { return false; }
    public static void writeRaw(byte[] buffer, int size) { }
    public static void write(final StatsEvent statsEvent) { if (statsEvent != null) statsEvent.release(); }
}

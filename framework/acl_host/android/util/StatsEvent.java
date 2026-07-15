// SPDX-License-Identifier: Apache-2.0
// Provenance: android.util.StatsEvent public/@SystemApi surface (AOSP SDK docs + the class's
// public method signatures, Apache-2.0). Clean-room boot-classpath shim: StatsEvent lives in the
// statsd apex (framework-statsd), absent on this host, so framework-minus-apex's
// com.android.internal.util.FrameworkStatsLog.buildStatsEvent(...) NoClassDefFoundErrors when a
// UI interaction logs a metric. acl does no statsd telemetry — the builder accepts every field and
// build() returns an empty event that StatsLog discards. No GPL-3.0 sources.
package android.util;

/** No-op metrics event: the builder swallows all fields; the event carries no bytes. */
public final class StatsEvent {
    public static final byte TYPE_INT = 0x00;
    public static final byte TYPE_LONG = 0x01;
    public static final byte TYPE_STRING = 0x02;
    public static final byte TYPE_LIST = 0x03;
    public static final byte TYPE_FLOAT = 0x04;
    public static final byte TYPE_BOOLEAN = 0x05;
    public static final byte TYPE_BYTE_ARRAY = 0x06;
    public static final byte TYPE_OBJECT = 0x07;
    public static final byte TYPE_KEY_VALUE_PAIRS = 0x08;
    public static final byte TYPE_ATTRIBUTION_CHAIN = 0x09;
    public static final byte TYPE_ERRORS = 0x0F;

    private static final byte[] EMPTY = new byte[0];
    private final int mAtomId;

    private StatsEvent(int atomId) { mAtomId = atomId; }

    public static Builder newBuilder() { return new Builder(); }

    public int getAtomId() { return mAtomId; }
    public byte[] getBytes() { return EMPTY; }
    public int getNumBytes() { return 0; }
    public void release() { }

    /** Accepts every field type and returns an empty StatsEvent from build(). */
    public static final class Builder {
        private int mAtomId;
        Builder() { }

        public Builder setAtomId(final int atomId) { mAtomId = atomId; return this; }
        public Builder writeBoolean(final boolean value) { return this; }
        public Builder writeInt(final int value) { return this; }
        public Builder writeLong(final long value) { return this; }
        public Builder writeFloat(final float value) { return this; }
        public Builder writeString(final String value) { return this; }
        public Builder writeByteArray(final byte[] value) { return this; }
        public Builder writeAttributionChain(final int[] uids, final String[] tags) { return this; }
        public Builder writeKeyValuePairs(final SparseIntArray intMap, final SparseLongArray longMap,
                final SparseArray<String> stringMap, final SparseArray<Float> floatMap) { return this; }
        public Builder addBooleanAnnotation(final byte annotationId, final boolean value) { return this; }
        public Builder addIntAnnotation(final byte annotationId, final int value) { return this; }
        public Builder usePooledBuffer() { return this; }
        public StatsEvent build() { return new StatsEvent(mAtomId); }
    }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.le.ScanSettings public surface + Builder (AOSP android.jar api-33
// stub; Android SDK docs). Boot-classpath shim (the bluetooth LE apex is absent from $BOOT). acl
// does not scan, so the settings are inert — the Builder accepts every configuration call an app
// makes so it can construct one to hand to the (no-op) scanner. No GPL-3.0 sources.
package android.bluetooth.le;

/** BLE scan settings. acl ignores them (scanning is a no-op); the Builder is call-compatible. */
public final class ScanSettings {
    public static final int SCAN_MODE_OPPORTUNISTIC = -1;
    public static final int SCAN_MODE_LOW_POWER = 0;
    public static final int SCAN_MODE_BALANCED = 1;
    public static final int SCAN_MODE_LOW_LATENCY = 2;

    public static final int CALLBACK_TYPE_ALL_MATCHES = 1;
    public static final int CALLBACK_TYPE_FIRST_MATCH = 2;
    public static final int CALLBACK_TYPE_MATCH_LOST = 4;

    public static final int MATCH_MODE_AGGRESSIVE = 1;
    public static final int MATCH_MODE_STICKY = 2;

    public static final int MATCH_NUM_ONE_ADVERTISEMENT = 1;
    public static final int MATCH_NUM_FEW_ADVERTISEMENT = 2;
    public static final int MATCH_NUM_MAX_ADVERTISEMENT = 3;

    public static final int PHY_LE_ALL_SUPPORTED = 255;

    ScanSettings() {}

    public static final class Builder {
        public Builder() {}
        public Builder setScanMode(int scanMode) { return this; }
        public Builder setCallbackType(int callbackType) { return this; }
        public Builder setReportDelay(long reportDelayMillis) { return this; }
        public Builder setMatchMode(int matchMode) { return this; }
        public Builder setNumOfMatches(int numOfMatches) { return this; }
        public Builder setLegacy(boolean legacy) { return this; }
        public Builder setPhy(int phy) { return this; }
        public ScanSettings build() { return new ScanSettings(); }
    }
}

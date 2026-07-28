// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.le.ScanFilter public surface + Builder (AOSP android.jar api-33
// stub; Android SDK docs). Boot-classpath shim (the bluetooth LE apex is absent from $BOOT). acl
// does not scan, so a filter carries no behavior — the Builder just needs to accept the calls an
// app makes so it can construct one to hand to the (no-op) scanner. No GPL-3.0 sources.
package android.bluetooth.le;

import android.os.ParcelUuid;

/** A BLE scan filter. acl ignores filters (scanning is a no-op); the Builder is call-compatible. */
public final class ScanFilter {
    ScanFilter() {}

    public static final class Builder {
        public Builder() {}
        public Builder setServiceUuid(ParcelUuid serviceUuid) { return this; }
        public Builder setServiceUuid(ParcelUuid serviceUuid, ParcelUuid mask) { return this; }
        public Builder setServiceData(ParcelUuid serviceDataUuid, byte[] serviceData) { return this; }
        public Builder setServiceData(ParcelUuid serviceDataUuid, byte[] serviceData, byte[] mask) { return this; }
        public Builder setDeviceAddress(String deviceAddress) { return this; }
        public Builder setDeviceName(String deviceName) { return this; }
        public Builder setManufacturerData(int manufacturerId, byte[] manufacturerData) { return this; }
        public Builder setManufacturerData(int manufacturerId, byte[] manufacturerData, byte[] mask) { return this; }
        public ScanFilter build() { return new ScanFilter(); }
    }
}

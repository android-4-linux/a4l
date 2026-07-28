// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.le.ScanResult public surface (AOSP android.jar api-33 stub; Android
// SDK docs). Boot-classpath shim (the bluetooth LE apex is absent from $BOOT). acl never produces a
// ScanResult (BLE scanning is a no-op), so this only needs to be a loadable type for method
// descriptors to resolve; the accessors return empty/zero should an app ever hold one. No GPL-3.0 sources.
package android.bluetooth.le;

import android.bluetooth.BluetoothDevice;

/** A single BLE scan result. acl never delivers one; present only so scan descriptors resolve. */
public final class ScanResult {
    private final BluetoothDevice device;
    private final int rssi;

    public ScanResult() { this.device = null; this.rssi = 0; }
    public ScanResult(BluetoothDevice device, int rssi) { this.device = device; this.rssi = rssi; }

    public BluetoothDevice getDevice() { return device; }
    public int getRssi() { return rssi; }
    public long getTimestampNanos() { return 0L; }
    public boolean isConnectable() { return false; }
}

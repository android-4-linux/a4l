// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.le.BluetoothLeScanner public surface (AOSP android.jar api-33 stub;
// Android SDK docs). acl has no BLE-scan backend, so a scan is an accepted no-op (no results are
// delivered to the callback) — enough for an app that starts a scan on launch (e.g. Sonos speaker
// discovery) not to crash on a missing scanner. No GPL-3.0 sources.
package android.bluetooth.le;

import java.util.List;

/**
 * Shim of {@link android.bluetooth.le.BluetoothLeScanner}. Returned by acl's BluetoothAdapter
 * .getBluetoothLeScanner(); its scan requests are accepted and silently do nothing (acl has no BLE
 * scanner). Sonos-style speaker discovery over BLE therefore finds nothing, but the app runs;
 * network (mDNS) discovery is backed separately by svc/nsd.
 */
public final class BluetoothLeScanner {
    public BluetoothLeScanner() {}

    public void startScan(ScanCallback callback) {}
    public void startScan(List<ScanFilter> filters, ScanSettings settings, ScanCallback callback) {}
    public int startScan(List<ScanFilter> filters, ScanSettings settings,
                         android.app.PendingIntent callbackIntent) {
        return 0;  // SCAN_SUCCESS-equivalent; no results ever delivered
    }

    public void stopScan(ScanCallback callback) {}
    public void stopScan(android.app.PendingIntent callbackIntent) {}
    public void flushPendingScanResults(ScanCallback callback) {}
}

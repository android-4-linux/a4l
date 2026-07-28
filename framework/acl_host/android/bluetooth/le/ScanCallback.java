// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.le.ScanCallback public surface (AOSP android.jar api-33 stub;
// Android SDK docs). The bluetooth LE classes live in the com.android.btservices apex, absent from
// $BOOT, so — like android.bluetooth.BluetoothAdapter — acl provides a clean-room boot-classpath
// shim. Apps subclass this to receive scan results; acl's scanner never delivers any. No GPL-3.0 sources.
package android.bluetooth.le;

import java.util.List;

/** Callback for BLE scan results. acl's BluetoothLeScanner is a no-op, so none of these fire. */
public abstract class ScanCallback {
    public static final int SCAN_FAILED_ALREADY_STARTED = 1;
    public static final int SCAN_FAILED_APPLICATION_REGISTRATION_FAILED = 2;
    public static final int SCAN_FAILED_INTERNAL_ERROR = 3;
    public static final int SCAN_FAILED_FEATURE_UNSUPPORTED = 4;
    public static final int SCAN_FAILED_OUT_OF_HARDWARE_RESOURCES = 5;
    public static final int SCAN_FAILED_SCANNING_TOO_FREQUENTLY = 6;

    public void onScanResult(int callbackType, ScanResult result) {}
    public void onBatchScanResults(List<ScanResult> results) {}
    public void onScanFailed(int errorCode) {}
}

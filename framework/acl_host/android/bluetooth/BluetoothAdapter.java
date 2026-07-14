// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.BluetoothAdapter public surface (AOSP android.jar api-33
// stubs, Apache-2.0); backed by the acl svc/bluetooth host BlueZ backend via
// org.acl.internal.BluetoothBridge. Clean-room shim on the boot classpath (the real class
// lives in an apex module, absent from $BOOT). No GPL-3.0 sources. See docs/bluetooth-scope.md.
package android.bluetooth;

import java.util.LinkedHashSet;
import java.util.Set;

/**
 * A minimal BluetoothAdapter that maps to the host BlueZ adapter (org.bluez.Adapter1) through
 * {@link org.acl.internal.BluetoothBridge}. P1 (read + basic mutation): state/address/name,
 * bonded devices, discovery. LE / advertising / RFCOMM sockets and the profile proxies are
 * follow-on phases (getProfileProxy returns false for now).
 */
public final class BluetoothAdapter {
    public static final int STATE_OFF = 10;
    public static final int STATE_TURNING_ON = 11;
    public static final int STATE_ON = 12;
    public static final int STATE_TURNING_OFF = 13;

    public static final int STATE_DISCONNECTED = 0;
    public static final int STATE_CONNECTING = 1;
    public static final int STATE_CONNECTED = 2;
    public static final int STATE_DISCONNECTING = 3;

    public static final int SCAN_MODE_NONE = 20;
    public static final int SCAN_MODE_CONNECTABLE = 21;
    public static final int SCAN_MODE_CONNECTABLE_DISCOVERABLE = 23;

    public static final int ERROR = Integer.MIN_VALUE;

    public static final String ACTION_STATE_CHANGED = "android.bluetooth.adapter.action.STATE_CHANGED";
    public static final String ACTION_DISCOVERY_STARTED = "android.bluetooth.adapter.action.DISCOVERY_STARTED";
    public static final String ACTION_DISCOVERY_FINISHED = "android.bluetooth.adapter.action.DISCOVERY_FINISHED";
    public static final String ACTION_SCAN_MODE_CHANGED = "android.bluetooth.adapter.action.SCAN_MODE_CHANGED";
    public static final String ACTION_LOCAL_NAME_CHANGED = "android.bluetooth.adapter.action.LOCAL_NAME_CHANGED";
    public static final String ACTION_REQUEST_ENABLE = "android.bluetooth.adapter.action.REQUEST_ENABLE";
    public static final String ACTION_REQUEST_DISCOVERABLE = "android.bluetooth.adapter.action.REQUEST_DISCOVERABLE";
    public static final String EXTRA_STATE = "android.bluetooth.adapter.extra.STATE";
    public static final String EXTRA_PREVIOUS_STATE = "android.bluetooth.adapter.extra.PREVIOUS_STATE";
    public static final String EXTRA_SCAN_MODE = "android.bluetooth.adapter.extra.SCAN_MODE";
    public static final String EXTRA_LOCAL_NAME = "android.bluetooth.adapter.extra.LOCAL_NAME";

    private static BluetoothAdapter sAdapter;

    /** The default adapter (deprecated static entry apps still use); null only with no host bus. */
    public static synchronized BluetoothAdapter getDefaultAdapter() {
        if (sAdapter == null) sAdapter = new BluetoothAdapter();
        return sAdapter;
    }

    BluetoothAdapter() {}

    public int getState() { return org.acl.internal.BluetoothBridge.adapterState(); }
    public int getLeState() { return getState(); }
    public boolean isEnabled() { return getState() == STATE_ON; }
    public boolean enable() { return org.acl.internal.BluetoothBridge.setPowered(true); }
    public boolean disable() { return org.acl.internal.BluetoothBridge.setPowered(false); }

    public String getAddress() {
        String a = org.acl.internal.BluetoothBridge.address();
        return (a == null || a.isEmpty()) ? "02:00:00:00:00:00" : a;
    }
    public String getName() { return org.acl.internal.BluetoothBridge.name(); }
    public boolean setName(String name) { return false; }

    public int getScanMode() { return SCAN_MODE_NONE; }
    public boolean isDiscovering() { return org.acl.internal.BluetoothBridge.isDiscovering(); }
    public boolean startDiscovery() { return org.acl.internal.BluetoothBridge.startDiscovery(); }
    public boolean cancelDiscovery() { return org.acl.internal.BluetoothBridge.stopDiscovery(); }

    public Set<BluetoothDevice> getBondedDevices() {
        LinkedHashSet<BluetoothDevice> set = new LinkedHashSet<>();
        for (String addr : org.acl.internal.BluetoothBridge.bondedAddresses()) {
            set.add(new BluetoothDevice(addr));
        }
        return set;
    }

    public BluetoothDevice getRemoteDevice(String address) { return new BluetoothDevice(address); }
    public BluetoothDevice getRemoteDevice(byte[] address) {
        if (address == null || address.length != 6) {
            throw new IllegalArgumentException("Bluetooth address must have 6 bytes");
        }
        StringBuilder sb = new StringBuilder(17);
        for (int i = 0; i < 6; i++) {
            if (i != 0) sb.append(':');
            sb.append(String.format("%02X", address[i] & 0xff));
        }
        return new BluetoothDevice(sb.toString());
    }

    public int getProfileConnectionState(int profile) { return STATE_DISCONNECTED; }

    // Profile proxies (A2DP/HEADSET/...) are P3; report unavailable so the app doesn't wait.
    public boolean getProfileProxy(android.content.Context context,
                                   BluetoothProfile.ServiceListener listener, int profile) {
        return false;
    }
    public void closeProfileProxy(int profile, BluetoothProfile proxy) {}

    public boolean isMultipleAdvertisementSupported() { return false; }
    public boolean isOffloadedFilteringSupported() { return false; }
    public boolean isOffloadedScanBatchingSupported() { return false; }

    /** Validate an "AA:BB:CC:DD:EE:FF" address, mirroring BluetoothAdapter.checkBluetoothAddress. */
    public static boolean checkBluetoothAddress(String address) {
        if (address == null || address.length() != 17) return false;
        for (int i = 0; i < 17; i++) {
            char c = address.charAt(i);
            if ((i % 3) == 2) {
                if (c != ':') return false;
            } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }
}

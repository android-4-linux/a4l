// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.BluetoothDevice public surface (AOSP android.jar api-33
// stubs, Apache-2.0); backed by the acl svc/bluetooth host BlueZ backend (org.bluez.Device1)
// via org.acl.internal.BluetoothBridge. Clean-room boot-classpath shim. No GPL-3.0 sources.
package android.bluetooth;

/** A remote device, keyed by address; properties resolve from the cached BlueZ Device1. */
public final class BluetoothDevice {
    public static final int BOND_NONE = 10;
    public static final int BOND_BONDING = 11;
    public static final int BOND_BONDED = 12;

    public static final int DEVICE_TYPE_UNKNOWN = 0;
    public static final int DEVICE_TYPE_CLASSIC = 1;
    public static final int DEVICE_TYPE_LE = 2;
    public static final int DEVICE_TYPE_DUAL = 3;

    public static final String ACTION_FOUND = "android.bluetooth.device.action.FOUND";
    public static final String ACTION_BOND_STATE_CHANGED = "android.bluetooth.device.action.BOND_STATE_CHANGED";
    public static final String ACTION_ACL_CONNECTED = "android.bluetooth.device.action.ACL_CONNECTED";
    public static final String ACTION_ACL_DISCONNECTED = "android.bluetooth.device.action.ACL_DISCONNECTED";
    public static final String EXTRA_DEVICE = "android.bluetooth.device.extra.DEVICE";
    public static final String EXTRA_NAME = "android.bluetooth.device.extra.NAME";
    public static final String EXTRA_BOND_STATE = "android.bluetooth.device.extra.BOND_STATE";

    private final String mAddress;

    BluetoothDevice(String address) { mAddress = address != null ? address : ""; }

    public String getAddress() { return mAddress; }
    public String getName() {
        String n = org.acl.internal.BluetoothBridge.deviceName(mAddress);
        return (n == null || n.isEmpty()) ? null : n;
    }
    public String getAlias() { return getName(); }
    public int getBondState() { return org.acl.internal.BluetoothBridge.deviceBondState(mAddress); }
    public int getType() { return DEVICE_TYPE_UNKNOWN; }
    public BluetoothClass getBluetoothClass() {
        return new BluetoothClass(org.acl.internal.BluetoothBridge.deviceClass(mAddress));
    }

    @Override public String toString() { return mAddress; }
    @Override public boolean equals(Object o) {
        return (o instanceof BluetoothDevice) && mAddress.equals(((BluetoothDevice) o).mAddress);
    }
    @Override public int hashCode() { return mAddress.hashCode(); }
}

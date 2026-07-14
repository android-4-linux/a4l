// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.BluetoothClass public surface (AOSP android.jar api-33
// stubs, Apache-2.0); the class-of-device bitfield from BlueZ Device1.Class. Clean-room
// boot-classpath shim. No GPL-3.0 sources.
package android.bluetooth;

/** The class-of-device bitfield (BlueZ Device1 "Class"), decoded per the Bluetooth CoD spec. */
public final class BluetoothClass {
    private final int mClass;

    public BluetoothClass(int deviceClass) { mClass = deviceClass; }

    public int getMajorDeviceClass() { return mClass & 0x1f00; }
    public int getDeviceClass() { return mClass & 0x1ffc; }
    public boolean hasService(int service) { return (mClass & service) != 0; }

    @Override public boolean equals(Object o) {
        return (o instanceof BluetoothClass) && mClass == ((BluetoothClass) o).mClass;
    }
    @Override public int hashCode() { return mClass; }
    @Override public String toString() { return Integer.toHexString(mClass); }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam to the svc/bluetooth host BlueZ backend; consumed by the
// clean-room android.bluetooth.* shims. No GPL-3.0 sources.
package org.acl.internal;

/**
 * The native bridge from the clean-room {@code android.bluetooth.*} shims to the acl
 * svc/bluetooth host BlueZ backend. All values are read off the backend's cached BlueZ object
 * model (adapter power/address/name/discovering + paired devices); mutations call BlueZ. Bound
 * by acl at host-ART boot under ACL_HAVE_SDBUS; absent -> the defaults below (adapter off).
 */
public final class BluetoothBridge {
    private BluetoothBridge() {}

    static native int nAdapterState();          // BluetoothAdapter.STATE_* (10 off / 12 on)
    static native String nAddress();
    static native String nName();
    static native boolean nIsDiscovering();
    static native String[] nBondedAddresses();
    static native String nDeviceName(String address);
    static native int nDeviceClass(String address);
    static native int nDeviceBondState(String address);  // BluetoothDevice.BOND_* (10/11/12)
    static native boolean nSetPowered(boolean on);
    static native boolean nStartDiscovery();
    static native boolean nStopDiscovery();

    // Safe wrappers (natives absent -> adapter off).
    public static int adapterState() {
        try { return nAdapterState(); } catch (Throwable t) { return 10 /* STATE_OFF */; }
    }
    public static String address() {
        try { String a = nAddress(); return a != null ? a : ""; } catch (Throwable t) { return ""; }
    }
    public static String name() {
        try { String n = nName(); return n != null ? n : ""; } catch (Throwable t) { return ""; }
    }
    public static boolean isDiscovering() {
        try { return nIsDiscovering(); } catch (Throwable t) { return false; }
    }
    public static String[] bondedAddresses() {
        try { String[] a = nBondedAddresses(); return a != null ? a : new String[0]; }
        catch (Throwable t) { return new String[0]; }
    }
    public static String deviceName(String address) {
        try { String n = nDeviceName(address); return n != null ? n : ""; } catch (Throwable t) { return ""; }
    }
    public static int deviceClass(String address) {
        try { return nDeviceClass(address); } catch (Throwable t) { return 0; }
    }
    public static int deviceBondState(String address) {
        try { return nDeviceBondState(address); } catch (Throwable t) { return 10 /* BOND_NONE */; }
    }
    public static boolean setPowered(boolean on) {
        try { return nSetPowered(on); } catch (Throwable t) { return false; }
    }
    public static boolean startDiscovery() {
        try { return nStartDiscovery(); } catch (Throwable t) { return false; }
    }
    public static boolean stopDiscovery() {
        try { return nStopDiscovery(); } catch (Throwable t) { return false; }
    }
}

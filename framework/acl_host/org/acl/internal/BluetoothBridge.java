// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam to the svc/bluetooth host BlueZ backend; consumed by the
// clean-room android.bluetooth.* shims. No GPL-3.0 sources.
package org.acl.internal;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;

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
    static native String[] nAllDevices();       // paired + discovered (for ACTION_FOUND diffing)
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
    public static String[] allDevices() {
        try { String[] a = nAllDevices(); return a != null ? a : new String[0]; }
        catch (Throwable t) { return new String[0]; }
    }

    // ── P2 broadcasts ────────────────────────────────────────────────────────────────────
    // The native svc/bluetooth backend fires onBluetoothChanged() on the D-Bus thread when the
    // adapter/devices change; hop to the app main thread and diff against the last-seen state to
    // emit the android.bluetooth ACTION_* broadcasts through the app Context's receiver bus.
    private static android.os.Handler sMain;
    private static boolean sInitialized;
    private static int sPrevState = BluetoothAdapter.STATE_OFF;
    private static boolean sPrevDiscovering;
    private static java.util.HashSet<String> sSeen = new java.util.HashSet<>();

    /** Called from the native change sink (D-Bus thread). */
    public static void onBluetoothChanged() {
        android.os.Handler h;
        synchronized (BluetoothBridge.class) {
            if (sMain == null) sMain = new android.os.Handler(android.os.Looper.getMainLooper());
            h = sMain;
        }
        h.post(new Runnable() { @Override public void run() { deliverChanges(); } });
    }

    private static void deliverChanges() {  // main thread
        final int state = adapterState();
        final boolean discovering = isDiscovering();
        final String[] all = allDevices();
        final java.util.HashSet<String> now = new java.util.HashSet<>();
        for (String a : all) now.add(a);

        if (!sInitialized) {  // baseline; don't replay the current world as "changes"
            sInitialized = true;
            sPrevState = state;
            sPrevDiscovering = discovering;
            sSeen = now;
            return;
        }

        if (state != sPrevState) {
            android.content.Intent i = new android.content.Intent(BluetoothAdapter.ACTION_STATE_CHANGED);
            i.putExtra(BluetoothAdapter.EXTRA_PREVIOUS_STATE, sPrevState);
            i.putExtra(BluetoothAdapter.EXTRA_STATE, state);
            HostBootstrap.deliverBroadcast(i);
            sPrevState = state;
        }
        if (discovering != sPrevDiscovering) {
            HostBootstrap.deliverBroadcast(new android.content.Intent(discovering
                    ? BluetoothAdapter.ACTION_DISCOVERY_STARTED
                    : BluetoothAdapter.ACTION_DISCOVERY_FINISHED));
            sPrevDiscovering = discovering;
        }
        // Newly-seen devices → ACTION_FOUND (getRemoteDevice yields a Parcelable BluetoothDevice).
        final BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
        for (String addr : all) {
            if (!sSeen.contains(addr)) {
                android.content.Intent i = new android.content.Intent(BluetoothDevice.ACTION_FOUND);
                i.putExtra(BluetoothDevice.EXTRA_DEVICE, adapter.getRemoteDevice(addr));
                i.putExtra(BluetoothDevice.EXTRA_NAME, deviceName(addr));
                HostBootstrap.deliverBroadcast(i);
            }
        }
        sSeen = now;
    }
}


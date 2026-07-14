// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.BluetoothA2dp public surface (AOSP android.jar api-33 stubs,
// Apache-2.0); backed by the acl svc/bluetooth host BlueZ backend (org.bluez Device1
// Connect/Disconnect + Connected + A2DP UUIDs) via org.acl.internal.BluetoothBridge. The
// profile proxy handed out by BluetoothAdapter.getProfileProxy(A2DP). Clean-room boot-classpath
// shim (the real one is an apex IBluetoothA2dp Binder proxy — ADR-3, no Binder). No GPL-3.0 sources.
package android.bluetooth;

import java.util.ArrayList;
import java.util.List;

/** The A2DP profile proxy: connection state + connect/disconnect, mapped to BlueZ Device1. */
public final class BluetoothA2dp implements BluetoothProfile {
    public static final int STATE_PLAYING = 10;
    public static final int STATE_NOT_PLAYING = 11;

    // Apps sometimes reflect into mService (the hidden IBluetoothA2dp Binder). We have no Binder
    // (ADR-3), so it stays null — such apps log "could not bind" and fall back to the public API.
    private final Object mService = null;

    BluetoothA2dp() {}

    @Override public int getConnectionState(BluetoothDevice device) {
        return (device != null && org.acl.internal.BluetoothBridge.deviceConnected(device.getAddress()))
                ? STATE_CONNECTED : STATE_DISCONNECTED;
    }

    @Override public List<BluetoothDevice> getConnectedDevices() {
        List<BluetoothDevice> list = new ArrayList<>();
        BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
        for (String addr : org.acl.internal.BluetoothBridge.a2dpConnected()) {
            list.add(adapter.getRemoteDevice(addr));
        }
        return list;
    }

    @Override public List<BluetoothDevice> getDevicesMatchingConnectionStates(int[] states) {
        if (states != null) {
            for (int s : states) if (s == STATE_CONNECTED) return getConnectedDevices();
        }
        return new ArrayList<>();
    }

    // @hide in AOSP; apps reach these via reflection. Route to BlueZ Device1.Connect/Disconnect.
    public boolean connect(BluetoothDevice device) {
        return device != null && org.acl.internal.BluetoothBridge.connectDevice(device.getAddress());
    }
    public boolean disconnect(BluetoothDevice device) {
        return device != null && org.acl.internal.BluetoothBridge.disconnectDevice(device.getAddress());
    }
    public boolean isA2dpPlaying(BluetoothDevice device) { return false; }
    public int getPriority(BluetoothDevice device) { return 0; }
    public boolean setPriority(BluetoothDevice device, int priority) { return false; }
}

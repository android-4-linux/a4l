// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.BluetoothProfile public surface (AOSP android.jar api-33
// stubs, Apache-2.0). Clean-room boot-classpath shim (constants + ServiceListener; the
// profile proxies themselves are a follow-on phase). No GPL-3.0 sources.
package android.bluetooth;

import java.util.List;

/** Profile constants + the ServiceListener callback interface used by getProfileProxy. */
public interface BluetoothProfile {
    int HEADSET = 1;
    int A2DP = 2;
    int HEALTH = 3;
    int GATT = 7;
    int GATT_SERVER = 8;

    int STATE_DISCONNECTED = 0;
    int STATE_CONNECTING = 1;
    int STATE_CONNECTED = 2;
    int STATE_DISCONNECTING = 3;

    int getConnectionState(BluetoothDevice device);
    List<BluetoothDevice> getConnectedDevices();
    List<BluetoothDevice> getDevicesMatchingConnectionStates(int[] states);

    interface ServiceListener {
        void onServiceConnected(int profile, BluetoothProfile proxy);
        void onServiceDisconnected(int profile);
    }
}

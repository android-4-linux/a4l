// SPDX-License-Identifier: Apache-2.0
// Provenance: android.bluetooth.BluetoothManager public surface (AOSP android.jar api-33
// stubs, Apache-2.0). The getSystemService(BLUETOOTH_SERVICE) Manager, wired through
// AppContext.getSystemService. Clean-room boot-classpath shim. No GPL-3.0 sources.
package android.bluetooth;

import java.util.ArrayList;
import java.util.List;

/** The system Bluetooth Manager: hands out the adapter and answers profile connection state. */
public final class BluetoothManager {
    public BluetoothManager() {}
    public BluetoothManager(android.content.Context context) {}

    public BluetoothAdapter getAdapter() { return BluetoothAdapter.getDefaultAdapter(); }

    public int getConnectionState(BluetoothDevice device, int profile) {
        return BluetoothProfile.STATE_DISCONNECTED;
    }
    public List<BluetoothDevice> getConnectedDevices(int profile) { return new ArrayList<>(); }
    public List<BluetoothDevice> getDevicesMatchingConnectionStates(int profile, int[] states) {
        return new ArrayList<>();
    }
}

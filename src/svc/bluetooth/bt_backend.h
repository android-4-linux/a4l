// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.bluetooth.BluetoothAdapter/BluetoothDevice public
// surface (AOSP android.jar api-33 stubs, Apache-2.0); host mapping from BlueZ
// (org.bluez Adapter1/Device1) D-Bus API docs; sdbus-c++ + cached-object-model
// pattern after jwinarske/sdbus-cpp-examples/src/bluez and
// jwinarske/bluez_native (both Apache-2.0). No GPL-3.0 sources. See
// docs/bluetooth-scope.md.
//
// svc/bluetooth translates the host Bluetooth daemon (BlueZ) into the slice of
// BluetoothAdapter/BluetoothDevice state Android apps read. The backend
// snapshots the BlueZ object tree once (GetManagedObjects) and keeps a small
// in-memory model live from PropertiesChanged / InterfacesAdded /
// InterfacesRemoved, so the (synchronous) Android getters answer off the cache
// with no D-Bus round-trip; the Java shims + the JNI seam speak only
// AndroidBtState.
#ifndef ACL_SVC_BLUETOOTH_BT_BACKEND_H
#define ACL_SVC_BLUETOOTH_BT_BACKEND_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acl::bt {

// BluetoothAdapter.STATE_* (android.bluetooth.BluetoothAdapter).
enum : int {
  kStateOff = 10,
  kStateTurningOn = 11,
  kStateOn = 12,
  kStateTurningOff = 13
};
// BluetoothDevice.BOND_* .
enum : int { kBondNone = 10, kBondBonding = 11, kBondBonded = 12 };

// One remote device (a BlueZ Device1), reduced to the
// android.bluetooth.BluetoothDevice slice.
struct BtDevice {
  std::string address;         // AA:BB:CC:DD:EE:FF (BluetoothDevice.getAddress)
  std::string name;            // Alias/Name (getName)
  std::string path;            // BlueZ object path (for Connect/Disconnect)
  int cls = 0;                 // Class (u) → BluetoothClass
  int bond_state = kBondNone;  // Paired → BOND_BONDED
  bool connected = false;      // Device1.Connected
  bool a2dp =
      false;  // advertises an A2DP UUID (sink/source) → a BluetoothA2dp device
};

// The Android-visible adapter truth, independent of the host daemon.
struct AndroidBtState {
  bool present = false;      // an adapter exists at all
  int state = kStateOff;     // STATE_ON when Powered, else STATE_OFF
  std::string address;       // getAddress()
  std::string name;          // getName() (Alias)
  bool discovering = false;  // isDiscovering()
  std::vector<BtDevice>
      devices;  // all known devices; getBondedDevices() filters bond_state
};

// A backend owns a D-Bus proxy to the host Bluetooth daemon. snapshot() answers
// the synchronous reads off the cache; the mutators call the daemon; set_sink()
// arms an async callback fired on every relevant change (→ ACTION_* broadcasts
// in L1).
class IBtBackend {
 public:
  virtual ~IBtBackend() = default;

  virtual const char* name() const noexcept = 0;
  virtual AndroidBtState snapshot() const = 0;

  virtual bool set_powered(bool on) = 0;
  virtual bool start_discovery() = 0;
  virtual bool stop_discovery() = 0;

  // Connect/disconnect a device by address (BlueZ Device1.Connect/Disconnect) —
  // the A2DP profile's connect(device)/disconnect(device).
  virtual bool connect_device(const std::string& address) = 0;
  virtual bool disconnect_device(const std::string& address) = 0;

  virtual void set_sink(
      std::function<void(const AndroidBtState&)> on_change) = 0;
};

// Probe the system bus and construct the backend that owns it: BlueZ when
// present (ACL_BT_BACKEND env override honored), else a Null backend (no
// adapter). Never null.
std::unique_ptr<IBtBackend> make_bt_backend();

}  // namespace acl::bt

#endif  // ACL_SVC_BLUETOOTH_BT_BACKEND_H

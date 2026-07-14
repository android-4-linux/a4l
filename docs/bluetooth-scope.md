<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: Android android.bluetooth.* public surface (AOSP android.jar api-33 stubs,
     Apache-2.0); host mapping from BlueZ (org.bluez) D-Bus API docs; sdbus-c++ usage +
     cached-object-model patterns after jwinarske/sdbus-cpp-examples/src/bluez and
     jwinarske/bluez_native (both Apache-2.0). No GPL-3.0 sources. -->

# android.bluetooth → BlueZ mapping — scope

## Problem

`android.bluetooth.BluetoothAdapter` is a **ClassNotFoundException** under host ART: unlike
`android.net.*` (which we vendor/shim), the bluetooth classes moved to a mainline module and are
absent from the `framework-minus-apex` boot classpath. a2dp.Vol (a Bluetooth A2DP volume app) dies
at `BluetoothAdapter.getDefaultAdapter()` during onCreate. So bluetooth needs **two** pieces, exactly
like connectivity did (`android.net.*` cluster + `svc/net`):

1. a clean-room **`android.bluetooth.*` cluster on the boot classpath**, and
2. a host backend, **`svc/bluetooth`**, translating BlueZ (`org.bluez`) D-Bus into it.

BlueZ is present and testable on desktop/device: this host runs `bluetoothd` with a real adapter
`/org/bluez/hci0` (`Powered=true`, `Address`, `Name`, `Discovering`, `StartDiscovery`/`StopDiscovery`).

## Architecture (mirror svc/net + android.net.*)

```
app → android.bluetooth.BluetoothAdapter (clean-room, boot classpath)
        │  native seam (BluetoothBridge / nGetAdapterState, nGetBondedDevices, …)
        ▼
     svc/bluetooth  (sdbus-c++)  ──►  org.bluez  (system bus)
        AndroidBtState ◄── Adapter1 / Device1 / (Media*) via ObjectManager
```

- `svc/bluetooth/bt_backend.{h,cpp}`: an `IBtBackend` + a **BlueZ** backend and a **Null** backend
  (adapter absent → `STATE_OFF`, empty bonded set), `make_bt_backend()` probing
  `NameHasOwner("org.bluez")` — the exact svc/net shape.
- **Cached object model (bluez_native pattern), not getProperty-per-call.** Unlike svc/net (one
  daemon, cheap reads), bluetooth is multi-device and the Android getters are synchronous, so the
  BlueZ backend snapshots the tree once via `GetManagedObjects` on connect into a small in-memory
  model (one adapter record + a device map keyed by object path / address), then keeps it live from
  `PropertiesChanged` (per object) + `InterfacesAdded`/`InterfacesRemoved` (device appear/disappear).
  Android reads (`isEnabled`, `getBondedDevices`, `isDiscovering`, device props) answer off the
  cache with no D-Bus round-trip; only mutations (`enable`, `startDiscovery`, `connect`) call BlueZ.
  The async event loop runs on the backend's own connection (`enterEventLoopAsync`), like svc/net.
- `svc/bluetooth/bt_jni.{h,cpp}`: `register_bt_natives(env)` binds the clean-room
  `org.acl.internal.BluetoothBridge` (or the shim classes' natives) to the backend.
- Clean-room `android.bluetooth.{BluetoothAdapter,BluetoothDevice,BluetoothManager,BluetoothClass,
  BluetoothProfile}` on the boot classpath (safe: real classes are apex-absent from `$BOOT`).
  `BluetoothManager` is a `getSystemService("bluetooth")` Manager, so it also wires through
  `AppContext.getSystemService` (like connectivity/notification).

## sdbus-c++ / BlueZ patterns (from sdbus-cpp-examples/src/bluez)

- `bluez_client.{h,cc}`: enumerate adapters + devices via **ObjectManager**
  (`org.freedesktop.DBus.ObjectManager.GetManagedObjects` on `/`), filtering by the
  `org.bluez.Adapter1` / `org.bluez.Device1` interface in each object's dict.
- Per-interface proxy headers (`adapter1.h`, `device1.h`, `battery1.h`, `media1.h`, `input1.h`,
  `gatt_*`): the same `createProxy` + `getProperty(...).onInterface(...)` + `callMethod(...)` +
  `uponSignal("PropertiesChanged")` shape svc/net already uses.
- `InterfacesAdded` / `InterfacesRemoved` (ObjectManager) + per-device `PropertiesChanged` drive the
  change sink → Android broadcasts (`ACTION_STATE_CHANGED`, `ACTION_DISCOVERY_*`, `ACTION_FOUND`).

## Mapping table (BlueZ → android.bluetooth)

| Android (BluetoothAdapter)            | BlueZ `org.bluez.Adapter1` on `/org/bluez/hciN`             |
|---------------------------------------|-------------------------------------------------------------|
| `getState()` / `isEnabled()`          | `Powered` (true → `STATE_ON`, false → `STATE_OFF`)          |
| `enable()` / `disable()`              | set `Powered` = true/false (writable)                        |
| `getAddress()`                        | `Address` ("8C:B8:7E:…")                                     |
| `getName()` / `setName()`             | `Alias` (writable; `Name` is read-only)                     |
| `isDiscovering()`                     | `Discovering`                                               |
| `startDiscovery()` / `cancelDiscovery()` | `StartDiscovery()` / `StopDiscovery()`                   |
| `getScanMode()` / discoverable        | `Discoverable` + `DiscoverableTimeout`                       |
| `getBondedDevices()`                  | ObjectManager → each `Device1` with `Paired=true`           |
| `getRemoteDevice(addr)`               | construct `BluetoothDevice` for `/org/bluez/hciN/dev_AA_…`  |

| Android (BluetoothDevice)             | BlueZ `org.bluez.Device1`                                    |
|---------------------------------------|-------------------------------------------------------------|
| `getAddress()` / `getName()`          | `Address` / `Alias`(or `Name`)                              |
| `getBondState()`                      | `Paired`/`Bonded`                                           |
| `getBluetoothClass()`                 | `Class` (u) → `BluetoothClass`                              |
| connection / UUIDs                    | `Connected`, `UUIDs`                                        |
| `createBond()` / `connect()`          | `Pair()` / `Connect()`                                      |

## a2dp.Vol specifics (the harder half)

a2dp.Vol reads the adapter (`getDefaultAdapter`/`isEnabled`/`getBondedDevices`/`getRemoteDevice`/
`getBluetoothClass`) **and** drives the **A2DP profile** via `android.bluetooth.IBluetoothA2dp`
(`getProfileProxy(A2DP)` → `BluetoothA2dp.connect`). That profile path is a **Binder** interface
(`IBluetoothA2dp$Stub$Proxy`) — no Binder (ADR-3). Map it at the Manager boundary instead: a
clean-room `BluetoothA2dp` whose `connect(device)`/`getConnectionState`/`getConnectedDevices` route
to `svc/bluetooth` → BlueZ `Device1.Connect()` (A2DP is a BlueZ-side profile) + `MediaControl1`/
`MediaTransport1` state, **not** a real `IBluetoothA2dp`. `getProfileProxy` returns our shim and
fires the `ServiceListener` immediately.

## Phasing

- **P1 — adapter read (unblocks a2dp.Vol onCreate):** clean-room `BluetoothAdapter`/`BluetoothManager`
  + `svc/bluetooth` BlueZ Adapter1 read: `getDefaultAdapter`/`isEnabled`/`getState`/`getAddress`/
  `getName`, `getBondedDevices` (Device1 Paired), `getRemoteDevice`, `checkBluetoothAddress`. Read-only.
- **P2 — discovery + device details + broadcasts:** `startDiscovery`/`cancelDiscovery`/`isDiscovering`,
  `BluetoothDevice` props + `BluetoothClass`, and the `ACTION_*` broadcasts (via the existing
  registerReceiver sticky/deliver seam) from `PropertiesChanged`/`InterfacesAdded`.
- **P3 — A2DP profile:** clean-room `BluetoothA2dp`/`BluetoothProfile` → BlueZ `Device1.Connect` +
  `Media*`. Enough for a2dp.Vol to connect/observe A2DP sinks. (LE / GATT / RFCOMM sockets are out of
  scope until an app needs them.)

## Scope / risk

Roughly svc/net-sized for P1 (one backend + Adapter1 reads + 5 clean-room classes), plus the
`android.bluetooth.*` boot-classpath cluster (bigger than `android.net.*` — Adapter/Device/Manager/
Class/Profile). P2/P3 add device enumeration and the profile shim. The A2DP profile is the only real
unknown (BlueZ exposes A2DP as a device profile + `Media*`, not a 1:1 of `IBluetoothA2dp`); everything
else is a direct property/method map. License: BlueZ is a **reference** (its D-Bus API docs), not
linked; sdbus-c++ stays LGPL-2.1 dynamic (policy-OK). No GPL-3.0 sources.

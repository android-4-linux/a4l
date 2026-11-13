<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: Android android.os.storage.StorageManager/StorageVolume + android.os.Environment
     public surface (AOSP framework api-33, Apache-2.0); host mapping from UDisks2
     (org.freedesktop.UDisks2) D-Bus API docs; sdbus-c++ usage + cached-object-model patterns
     after jwinarske/sdbus-cpp-examples and jwinarske/bluez_native (both Apache-2.0). No GPL-3.0 sources. -->

# svc/storage — IStorageManager → UDisks2 scope

`svc/storage` maps the slice of Android's storage API that unmodified apps read onto the host
UDisks2 daemon (`org.freedesktop.UDisks2`), via sdbus-c++, mirroring `svc/battery` (UPower),
`svc/bluetooth` (BlueZ), and `svc/net`. It is **read-only enumeration** — a4l never mounts,
unmounts, formats, or ejects host storage.

## What pulls it in

An app rarely calls `IStorageManager` directly; it calls `StorageManager.getStorageVolumes()`,
`getPrimaryStorageVolume()`, or `Environment.getExternalStorageState()`. In the real host-ART
framework these all funnel to:

```
Environment.getExternalStorageState()
  → StorageManager.getStorageVolume(File)
    → IStorageManager.getVolumeList(int userId, String callingPackage, int flags)
```

`IStorageManager` resolves through `ServiceManager.getService("mount")`. a4l has no Binder
(ADR-3), so without a seeded `"mount"` service that lookup returns `null` and `getVolumeList`
NPEs — the wall a2dp.Vol hit after its `AudioManager` gap cleared.

## The mapping

`HostBootstrap` seeds `"mount"` → a no-Binder `IStorageManager` proxy (exactly like
`"audio"`→IAudioService, `"notification"`→INotificationManager). Its `getVolumeList` returns a
`StorageVolume[]`:

| StorageVolume            | Source                                                              |
|--------------------------|---------------------------------------------------------------------|
| `[0]` primary, emulated  | **synthesized in Java** from the app Context — rooted at the app's XDG external storage (`getExternalFilesDir(null)/..`), `removable=false`, `emulated=true`, `state="mounted"`. So `getExternalStorageState`/`getPrimaryStorageVolume` resolve and reads/writes land where a4l already roots external files. |
| `[1..]` removable        | **UDisks2** — each mounted `org.freedesktop.UDisks2.Filesystem` whose `Block` is not `HintSystem`/`HintIgnore` and whose `Drive` is `Removable`/`MediaRemovable`/`Ejectable` (USB sticks, SD cards): `path`=`MountPoints[0]`, `description`=`IdLabel` (else Drive `Model`), `fsUuid`=`IdUUID`, `removable=true`, `emulated=false`, `state="mounted"`. |

The backend snapshots the UDisks2 object tree once (`ObjectManager.GetManagedObjects`) and keeps
the cache live from `InterfacesAdded`/`InterfacesRemoved`, so synchronous Android reads answer
off the cache with no D-Bus round-trip. Plug/unplug fires `StorageBridge.onStorageChanged()`,
which diffs mount points on the app main thread and emits `ACTION_MEDIA_MOUNTED` /
`ACTION_MEDIA_UNMOUNTED` through the app Context's receiver bus.

`StorageVolume` has no public constructor in the api-33 stub jar, so `StorageBridge` builds the
real host-ART class through its 14-arg hidden constructor (located by shape for resilience),
falling back to no-arg + reflective private-field set, then to dropping the volume.

## Out of scope (return defaults / no-ops)

- **All mutating operations** — `mount`/`unmount`/`format`/`partitionCreate`/`benchmark`, OBB
  mount, disk/volume management. UDisks2 gates these behind polkit and they are destructive; a
  compat layer enumerates host storage, it does not reformat it.
- Storage quotas / `StorageStatsManager`, app-data isolation, scoped-storage/FUSE internals,
  `getDisks()`/`getVolumeRecords()` — default (empty arrays / zero).

## Verify

```sh
./build/src/svc/storage/acl-storage-probe        # prints backend + removable volumes; plug a USB stick
```

Backend selection honors `ACL_STORAGE_BACKEND` (`udisks2` | anything else → Null). Built only
under `ACL_HAVE_SDBUS`; absent → the Null backend (just the synthesized primary volume).

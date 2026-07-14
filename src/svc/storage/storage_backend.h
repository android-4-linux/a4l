// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.os.storage.StorageManager/StorageVolume public surface
// (AOSP framework api-33, Apache-2.0); host mapping from UDisks2 (org.freedesktop.UDisks2
// Block/Filesystem/Drive) D-Bus API docs; sdbus-c++ + cached-object-model pattern after
// jwinarske/sdbus-cpp-examples and jwinarske/bluez_native (both Apache-2.0). No GPL-3.0 sources.
// See docs/storage-scope.md.
//
// svc/storage translates the host storage daemon (UDisks2) into the slice of
// StorageVolume state Android apps read. Scope is READ-ONLY enumeration of mounted
// REMOVABLE filesystems (USB sticks, SD cards) — the app's primary/emulated volume is
// synthesized from the app Context in Java, not sourced here. The backend snapshots the
// UDisks2 object tree once (GetManagedObjects) and keeps a small in-memory model live from
// InterfacesAdded/InterfacesRemoved/PropertiesChanged, so the (synchronous) Android getters
// answer off the cache with no D-Bus round-trip. No mount/unmount/format operations (those
// need polkit auth and are destructive) — those stay out of scope.
#ifndef ACL_SVC_STORAGE_STORAGE_BACKEND_H
#define ACL_SVC_STORAGE_STORAGE_BACKEND_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acl::storage {

// One mounted removable filesystem (a UDisks2 Block+Filesystem whose Drive is removable),
// reduced to the android.os.storage.StorageVolume slice acl exposes.
struct StorageVol {
  std::string id;           // synthetic volume id ("public:<uuid>") → StorageVolume mId
  std::string path;         // first MountPoint → getDirectory()/getPath()
  std::string description;  // IdLabel (else Drive Model) → getDescription()
  std::string fs_uuid;      // IdUUID → getUuid()
  std::string fs_type;      // IdType (vfat/exfat/ext4/…) — informational
  std::string state;        // "mounted"
};

// The Android-visible removable-storage truth, independent of the host daemon.
struct AndroidStorageState {
  std::vector<StorageVol> volumes;  // mounted removable filesystems (may be empty)
};

// A backend owns a D-Bus proxy to the host storage daemon. snapshot() answers the
// synchronous reads off the cache; set_sink() arms an async callback fired on every
// relevant change (→ ACTION_MEDIA_* broadcasts in Java).
class IStorageBackend {
 public:
  virtual ~IStorageBackend() = default;

  virtual const char* name() const noexcept = 0;
  virtual AndroidStorageState snapshot() const = 0;

  virtual void set_sink(std::function<void(const AndroidStorageState&)> on_change) = 0;
};

// Probe the system bus and construct the backend that owns it: UDisks2 when present
// (ACL_STORAGE_BACKEND env override honored), else a Null backend (no removable media).
// Never null.
std::unique_ptr<IStorageBackend> make_storage_backend();

}  // namespace acl::storage

#endif  // ACL_SVC_STORAGE_STORAGE_BACKEND_H

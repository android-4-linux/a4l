// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool; prints the svc/storage backend + enumerated
// removable volumes. No GPL-3.0 sources.
//
// Prints which host storage daemon acl selects and the derived removable
// StorageVolume list (mount point, label, fs uuid/type), so the UDisks2 ->
// android.os.storage.StorageVolume mapping can be eyeballed on desktop/device.
// Plug in a USB stick to see it appear.
#include <cstdio>

#include "svc/storage/storage_backend.h"

int main() {
  auto backend = acl::storage::make_storage_backend();
  const acl::storage::AndroidStorageState s = backend->snapshot();
  std::printf("acl storage backend: %s\n", backend->name());
  std::printf("  removable volumes (%zu):\n", s.volumes.size());
  for (const acl::storage::StorageVol& v : s.volumes)
    std::printf("    %-24s  \"%s\"  uuid=%s  type=%s  state=%s\n",
                v.path.c_str(), v.description.c_str(), v.fs_uuid.c_str(),
                v.fs_type.c_str(), v.state.c_str());
  return 0;
}

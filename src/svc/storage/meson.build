# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; host D-Bus storage mapping to
# android.os.storage.StorageManager/StorageVolume. No GPL-3.0 sources.
#
# svc/storage — storage backend: translate the host UDisks2 daemon (org.freedesktop.
# UDisks2 Block/Filesystem/Drive) into the removable StorageVolume list via a cached
# object model. Read-only enumeration (no mount/unmount/format). Built only when sdbus-c++
# is present (LGPL-2.1, dynamically linked = policy-OK); otherwise svc/storage is absent and
# callers fall back to the Null backend (no removable media).
storage_lib = static_library(
  'acl_svc_storage',
  files('storage_backend.cpp', 'storage_jni.cpp'),
  include_directories : [config_inc, core_inc],
  # fmt via log/log.h; sdbus-c++ D-Bus client; jni_dep for the StorageBridge seam.
  dependencies : [fmt_dep, sdbus_dep, jni_dep],
)
module_libs += storage_lib

# Dev tool: print the selected backend + enumerated removable volumes. Handy on desktop to
# verify the UDisks2 -> StorageVolume mapping (plug in a USB stick and re-run).
executable(
  'acl-storage-probe',
  files('storage_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [storage_lib, acl_log],   # acl_log provides the log facade impl
  dependencies : [fmt_dep, sdbus_dep],
  install : false,
)

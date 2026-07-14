// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.StorageBridge (clean-room) to the
// svc/storage host UDisks2 backend. No GPL-3.0 sources.
#ifndef ACL_SVC_STORAGE_STORAGE_JNI_H
#define ACL_SVC_STORAGE_STORAGE_JNI_H

#include <jni.h>

namespace acl::storage {

// Create the host storage backend (make_storage_backend) and RegisterNatives on
// org.acl.internal.StorageBridge. Best-effort: on any failure the class keeps no native and
// StorageBridge reports no removable volumes (the app still gets its synthesized primary
// volume). Safe to call once at host-ART boot, after the acl boot classpath is up.
void register_storage_natives(JNIEnv* env);

}  // namespace acl::storage

#endif  // ACL_SVC_STORAGE_STORAGE_JNI_H

// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.KeystoreBridge to the
// svc/keystore Secret Service backend. No GPL-3.0 sources.
#ifndef ACL_SVC_KEYSTORE_KEYSTORE_JNI_H
#define ACL_SVC_KEYSTORE_KEYSTORE_JNI_H

#include <jni.h>

namespace acl::keystore {

// Create the keystore backend (make_keystore_backend) and RegisterNatives on
// org.acl.internal.KeystoreBridge (nStore / nLoad / nContains / nErase /
// nAliases), which acl's AndroidKeyStore JCE provider persists key material
// through. Best-effort: on any failure the class keeps no native and the
// provider can fall back / report no keys. Call once at host-ART boot, after
// the acl boot classpath is up.
void register_keystore_natives(JNIEnv* env);

}  // namespace acl::keystore

#endif  // ACL_SVC_KEYSTORE_KEYSTORE_JNI_H

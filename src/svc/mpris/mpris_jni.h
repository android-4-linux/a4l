// SPDX-License-Identifier: Apache-2.0
// Provenance: original. No GPL-3.0 sources.
#ifndef ACL_SVC_MPRIS_MPRIS_JNI_H
#define ACL_SVC_MPRIS_MPRIS_JNI_H

#include <jni.h>

namespace acl::mpris {

// Binds org.acl.internal.MprisBridge's natives and its command callback.
void register_mpris_natives(JNIEnv* env);

}  // namespace acl::mpris

#endif  // ACL_SVC_MPRIS_MPRIS_JNI_H

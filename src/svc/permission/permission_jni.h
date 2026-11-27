// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.HostPermissions to
// svc/permission (the XDG permission store). No GPL-3.0 sources.
#ifndef ACL_SVC_PERMISSION_PERMISSION_JNI_H
#define ACL_SVC_PERMISSION_PERMISSION_JNI_H

#include <jni.h>

namespace acl::permission {

// RegisterNatives on org.acl.internal.HostPermissions (nDeviceAllowed). Safe to
// call once at host-ART boot; builds with or without sdbus-c++.
void register_permission_natives(JNIEnv* env);

}  // namespace acl::permission

#endif  // ACL_SVC_PERMISSION_PERMISSION_JNI_H

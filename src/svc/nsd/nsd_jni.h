// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.NsdBridge to the svc/nsd
// host discovery backend (Avahi mDNS/DNS-SD). No GPL-3.0 sources.
#ifndef ACL_SVC_NSD_NSD_JNI_H
#define ACL_SVC_NSD_NSD_JNI_H

#include <jni.h>

namespace acl::nsd {

// Create the host discovery backend (make_nsd_backend) and RegisterNatives on
// org.acl.internal.NsdBridge, arming the sinks that forward browse/resolve
// events back into Java. Best-effort: on failure the class keeps no native and
// NsdManager reports discovery as unavailable. Safe to call once at host-ART
// boot.
void register_nsd_natives(JNIEnv* env);

}  // namespace acl::nsd

#endif  // ACL_SVC_NSD_NSD_JNI_H

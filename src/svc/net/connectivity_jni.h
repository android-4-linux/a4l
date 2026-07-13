// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding android.net.ConnectivityManager (clean-room
// shim) to the svc/net host connectivity backend. No GPL-3.0 sources.
#ifndef ACL_SVC_NET_CONNECTIVITY_JNI_H
#define ACL_SVC_NET_CONNECTIVITY_JNI_H

#include <jni.h>

namespace acl::net {

// Create the host connectivity backend (make_net_backend) and RegisterNatives on
// android.net.ConnectivityManager (nGetState). Best-effort: on any failure the
// class simply keeps no native and callers see the offline default. Safe to call
// once at host-ART boot, after the boot classpath (with the acl net shims) is up.
void register_connectivity_natives(JNIEnv* env);

}  // namespace acl::net

#endif  // ACL_SVC_NET_CONNECTIVITY_JNI_H

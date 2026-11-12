// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.PowerBridge to the
// svc/power host session/power backend (systemd-logind + power-profiles +
// thermal sysfs). No GPL-3.0 sources.
#ifndef ACL_SVC_POWER_POWER_JNI_H
#define ACL_SVC_POWER_POWER_JNI_H

#include <jni.h>

namespace acl::power {

// Create the host power backend (make_power_backend) and RegisterNatives on
// org.acl.internal.PowerBridge (nSnapshot / nAcquireWakeLock /
// nReleaseWakeLock). Best-effort: on any failure the class keeps no native and
// PowerBridge falls back to awake defaults. Safe to call once at host-ART boot,
// after the acl boot classpath is up.
void register_power_natives(JNIEnv* env);

}  // namespace acl::power

#endif  // ACL_SVC_POWER_POWER_JNI_H

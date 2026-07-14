// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.BatteryBridge to the
// svc/battery host power backend (UPower). No GPL-3.0 sources.
#ifndef ACL_SVC_BATTERY_BATTERY_JNI_H
#define ACL_SVC_BATTERY_BATTERY_JNI_H

#include <jni.h>

namespace acl::battery {

// Create the host battery backend (make_battery_backend) and RegisterNatives on
// org.acl.internal.BatteryBridge (nSnapshot / nTechnology). Best-effort: on any
// failure the class keeps no native and BatteryBridge falls back to defaults. Safe
// to call once at host-ART boot, after the acl boot classpath is up.
void register_battery_natives(JNIEnv* env);

}  // namespace acl::battery

#endif  // ACL_SVC_BATTERY_BATTERY_JNI_H

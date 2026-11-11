// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl JNI seam (mirrors svc/battery/battery_jni.h).
// Bridges org.acl.internal.AclSensorManager to acl::svc::sensor::SensorHub. No
// GPL-3.0 sources.
#ifndef ACL_SVC_SENSOR_SENSOR_JNI_H
#define ACL_SVC_SENSOR_SENSOR_JNI_H

#include <jni.h>

namespace acl::svc::sensor {

// Register the org.acl.internal.AclSensorManager natives (enumeration +
// enable/disable) and wire the hub's event listener to
// AclSensorManager.onSensorEvent. Called once at VM boot. JNI_OK on success
// (JNI_ERR if the shim class isn't on the classpath — best-effort, like the
// other svc JNIs).
jint register_sensor_natives(JNIEnv* env);

}  // namespace acl::svc::sensor

#endif  // ACL_SVC_SENSOR_SENSOR_JNI_H

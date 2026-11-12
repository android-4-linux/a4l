// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl JNI seam (mirrors svc/sensor/sensor_jni.h). Binds
// the acl::svc::location::LocationHub to org.acl.internal.AclLocationManager.
// No GPL-3.0 sources.

#ifndef ACL_SVC_LOCATION_LOCATION_JNI_H
#define ACL_SVC_LOCATION_LOCATION_JNI_H

#include <jni.h>

namespace acl::svc::location {

// Register org.acl.internal.AclLocationManager's native methods and wire the
// hub's fix listener to its static onLocationChanged up-call. Best-effort: logs
// and returns JNI_ERR if the class is not on the boot classpath. Call once from
// the runtime after the framework is up.
jint register_location_natives(JNIEnv* env);

}  // namespace acl::svc::location

#endif  // ACL_SVC_LOCATION_LOCATION_JNI_H

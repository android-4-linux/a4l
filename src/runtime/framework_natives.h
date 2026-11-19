// SPDX-License-Identifier: Apache-2.0
// Provenance: docs/framework-bootstrap.md §A3 (native underside); JNI Functions
// docs (RegisterNatives). No GPL-3.0 sources.
#ifndef ACL_RUNTIME_FRAMEWORK_NATIVES_H
#define ACL_RUNTIME_FRAMEWORK_NATIVES_H

#include <jni.h>

namespace acl::runtime {

// Register acl C++ implementations of android.* native methods against the
// framework classes present on the boot classpath (the JNI "native underside").
// Called once after the VM starts. Classes not on the classpath are skipped, so
// this is a no-op when no framework is vendored.
void register_framework_natives(JNIEnv* env);

// Register only the acl-supplied natives that the host libandroid_runtime does
// NOT provide (e.g. android.hardware.HardwareBuffer, backed by a gbm dma-buf).
// register_framework_natives calls this; the ART-native path (which uses
// libandroid_runtime for the rest) must call it too, as a supplement.
void register_acl_supplemental_natives(JNIEnv* env);

// Gives android.util.Log's write to a4l's log where liblog's would not be
// ART's: a system with a liblog of its own (one that writes to syslog, on some
// boards) whose symbols a system library has loaded ahead of ART's. An app's
// log lines then went wherever that library sends them and not to a4l's
// output. Does nothing where ART's liblog is the one bound. For the ART path,
// once libandroid_runtime has registered its natives.
void route_displaced_android_log(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_FRAMEWORK_NATIVES_H

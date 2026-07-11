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

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_FRAMEWORK_NATIVES_H

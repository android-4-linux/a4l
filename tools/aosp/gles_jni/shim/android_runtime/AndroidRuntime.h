// SPDX-License-Identifier: Apache-2.0
// Provenance: acl shim. Minimal stand-in for AOSP frameworks/base/core/jni/include/
// android_runtime/AndroidRuntime.h — declares ONLY AndroidRuntime::registerNativeMethods,
// the sole symbol the copied glgen android_opengl_GLES*.cpp reference from it. Avoids
// pulling in libandroid_runtime (Binder/AndroidRuntime/JavaVM boot machinery) into acl's
// host GLES JNI. Implemented in gles_shim.cpp as a thin JNIEnv::RegisterNatives. No GPL-3.0 sources.
#ifndef ACL_GLES_JNI_SHIM_ANDROID_RUNTIME_H
#define ACL_GLES_JNI_SHIM_ANDROID_RUNTIME_H

#include <jni.h>

namespace android {
class AndroidRuntime {
 public:
  static int registerNativeMethods(JNIEnv* env, const char* className,
                                   const JNINativeMethod* gMethods, int numMethods);
};
}  // namespace android

#endif  // ACL_GLES_JNI_SHIM_ANDROID_RUNTIME_H

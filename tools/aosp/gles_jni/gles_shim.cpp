// SPDX-License-Identifier: Apache-2.0
// Provenance: acl shim. Implements AndroidRuntime::registerNativeMethods (the one symbol
// the copied glgen android_opengl_GLES*.cpp use from android_runtime/AndroidRuntime.h) as a
// thin JNIEnv::RegisterNatives, and exposes register_acl_gles — the single entry acl calls
// to register android.opengl.GLES20 + GLES30 onto host Mesa libGLESv2. No GPL-3.0 sources.

#include <jni.h>

#include "android_runtime/AndroidRuntime.h"

// Defined in the copied glgen files (android_opengl_GLES20.cpp / GLES30.cpp) and the GLES1
// GL10/GL11 bindings (com_google_android_gles_jni_GLImpl.cpp — libGDX's checkGL20 inits GLImpl).
extern int register_android_opengl_jni_GLES20(JNIEnv* env);
extern int register_android_opengl_jni_GLES30(JNIEnv* env);
extern int register_com_google_android_gles_jni_GLImpl(JNIEnv* env);

namespace android {
int AndroidRuntime::registerNativeMethods(JNIEnv* env, const char* className,
                                          const JNINativeMethod* gMethods, int numMethods) {
  jclass clazz = env->FindClass(className);
  if (clazz == nullptr) return JNI_ERR;
  const int rc = env->RegisterNatives(clazz, gMethods, numMethods);
  env->DeleteLocalRef(clazz);
  return rc;
}
}  // namespace android

// acl entry: register the android.opengl.GLES20/GLES30 natives. Returns 0 on success, or the
// first failing register's JNI_ERR. The class's <clinit> then runs _nativeClassInit.
extern "C" int register_acl_gles(JNIEnv* env) {
  if (register_android_opengl_jni_GLES20(env) != 0) return JNI_ERR;
  if (register_android_opengl_jni_GLES30(env) != 0) return JNI_ERR;
  if (register_com_google_android_gles_jni_GLImpl(env) != 0) return JNI_ERR;
  return 0;
}

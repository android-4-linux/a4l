// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; the same checks as the library
// tools/build-ndk-repro.sh builds, without what that one needs from a4l's shim
// libraries. No GPL-3.0 sources.
//
// The boundary between a bionic library and the host, for `acl-ndk-probe
// --boundary` and `--jni`: a call up into the host and back with the
// library's thread-local intact; a JNI_OnLoad that calls GetEnv, GetVersion,
// FindClass and RegisterNatives; and a registered native, reached through
// a4l's down-call stub, that makes a variadic call back up.
#include <jni.h>

static __thread volatile int t_marker;

// Expected through BionicRuntime::run_boundary_selftest with cb(x) = x * 2:
// cb(x) + 1000.
int acl_ndk_boundary(int (*cb)(int), int x) {
  t_marker = 7;
  int r = cb(x);
  return r + (t_marker == 7 ? 1000 : 0);
}

// GetStaticMethodID, then CallStaticIntMethod(helper, 20, 30): 4250.
static jint the_registered(JNIEnv* env, jclass clazz) {
  t_marker = 55;
  jmethodID mid = (*env)->GetStaticMethodID(env, clazz, "helper", "(II)I");
  jint sum = (*env)->CallStaticIntMethod(env, clazz, mid, 20, 30);
  return 4200 + sum + (t_marker == 55 ? 0 : 100000);
}

// Twelve integer arguments with env and class, and nine doubles: more of each
// than there are registers for on x86_64 or arm64, so the last ones are on the
// stack, which the down-call stub must leave where this function looks.
static jlong many(JNIEnv* env,
                  jclass clazz,
                  jint a1,
                  jint a2,
                  jint a3,
                  jint a4,
                  jint a5,
                  jint a6,
                  jint a7,
                  jint a8,
                  jint a9,
                  jint a10,
                  jdouble d1,
                  jdouble d2,
                  jdouble d3,
                  jdouble d4,
                  jdouble d5,
                  jdouble d6,
                  jdouble d7,
                  jdouble d8,
                  jdouble d9) {
  (void)env;
  (void)clazz;
  const jlong ints = a1 * 1 + a2 * 2 + a3 * 3 + a4 * 4 + a5 * 5 + a6 * 6 +
                     a7 * 7 + a8 * 8 + a9 * 9 + a10 * 10;
  const jdouble doubles = d1 * 1 + d2 * 2 + d3 * 3 + d4 * 4 + d5 * 5 + d6 * 6 +
                          d7 * 7 + d8 * 8 + d9 * 9;
  return ints + (jlong)doubles * 1000;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env = 0;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK)
    return -1;
  (void)(*env)->GetVersion(env);
  jclass c = (*env)->FindClass(env, "com/acl/ndktest/Main");
  JNINativeMethod m[2] = {
      {"answer", "()I", (void*)the_registered},
      {"many", "(IIIIIIIIIIDDDDDDDDD)J", (void*)many},
  };
  if ((*env)->RegisterNatives(env, c, m, 2) != JNI_OK)
    return -1;
  t_marker = 33;
  return JNI_VERSION_1_6;
}

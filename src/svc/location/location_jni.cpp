// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl JNI seam (mirrors svc/sensor/sensor_jni.cpp). Delivers
// acl::svc::location::LocationHub fixes to org.acl.internal.AclLocationManager. No GPL-3.0 sources.

#include "svc/location/location_jni.h"

// JNINativeMethod.{name,signature} are char* (not const); the literal method table is read-only.
#pragma GCC diagnostic ignored "-Wwrite-strings"

#include <cstddef>

#include "log/log.h"
#include "svc/location/location.h"

namespace acl::svc::location {
namespace {
constexpr const char* kTag = "acl.svc.location.jni";

JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;         // global ref to AclLocationManager
jmethodID g_on_location = nullptr;  // static onLocationChanged(DDDDDDJZZZ)V

// android.location.LocationManager.isProviderEnabled backing: does a real source exist?
jboolean nAvailable(JNIEnv*, jclass) {
  return LocationHub::instance().available() ? JNI_TRUE : JNI_FALSE;
}

// requestLocationUpdates: begin host updates (idempotent). Returns whether a source is delivering.
jboolean nStart(JNIEnv*, jclass) {
  return LocationHub::instance().start() ? JNI_TRUE : JNI_FALSE;
}

void nStop(JNIEnv*, jclass) { LocationHub::instance().stop(); }

// Hub listener (runs on the source's D-Bus loop thread): push one fix up to Java. The loop thread
// attaches to the JVM as a daemon once (it lives for the process), then stays attached.
void deliver(const LocationFix& f) {
  if (g_vm == nullptr || g_cls == nullptr || g_on_location == nullptr) return;
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return;
  }
  env->CallStaticVoidMethod(g_cls, g_on_location, f.latitude, f.longitude, f.accuracy_m,
                            f.altitude_m, f.speed_mps, f.bearing_deg,
                            static_cast<jlong>(f.timestamp_ms),
                            f.has_altitude ? JNI_TRUE : JNI_FALSE,
                            f.has_speed ? JNI_TRUE : JNI_FALSE,
                            f.has_bearing ? JNI_TRUE : JNI_FALSE);
  if (env->ExceptionCheck()) env->ExceptionClear();
}

const JNINativeMethod kMethods[] = {
    {"nAvailable", "()Z", reinterpret_cast<void*>(nAvailable)},
    {"nStart", "()Z", reinterpret_cast<void*>(nStart)},
    {"nStop", "()V", reinterpret_cast<void*>(nStop)},
};
}  // namespace

jint register_location_natives(JNIEnv* env) {
  env->GetJavaVM(&g_vm);
  jclass c = env->FindClass("org/acl/internal/AclLocationManager");
  if (c == nullptr) { env->ExceptionClear(); log::w(kTag, "AclLocationManager not on classpath"); return JNI_ERR; }
  g_cls = static_cast<jclass>(env->NewGlobalRef(c));
  g_on_location = env->GetStaticMethodID(g_cls, "onLocationChanged", "(DDDDDDJZZZ)V");
  if (g_on_location == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "AclLocationManager.onLocationChanged missing");
    return JNI_ERR;
  }
  const jint rc = env->RegisterNatives(g_cls, kMethods, static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
  env->DeleteLocalRef(c);
  if (rc != JNI_OK) { log::w(kTag, "RegisterNatives failed (rc={})", rc); return rc; }
  LocationHub::instance().set_listener(deliver);
  log::i(kTag, "LocationManager natives registered");
  return JNI_OK;
}

}  // namespace acl::svc::location

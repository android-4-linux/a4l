// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl JNI seam (mirrors svc/battery/battery_jni.cpp).
// Delivers acl::svc::sensor::SensorHub samples to
// org.acl.internal.AclSensorManager. No GPL-3.0 sources.

#include "svc/sensor/sensor_jni.h"

// JNINativeMethod.{name,signature} are char* (not const); the literal method
// table is read-only.
#pragma GCC diagnostic ignored "-Wwrite-strings"

#include <cstddef>

#include "log/log.h"
#include "svc/sensor/sensor.h"

namespace acl::svc::sensor {
namespace {
constexpr const char* kTag = "acl.svc.sensor.jni";

JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;          // global ref to AclSensorManager
jmethodID g_on_event = nullptr;  // static onSensorEvent(int handle, float[]
                                 // values, int acc, long ts)

const SensorDesc* desc_at(jint i) {
  const auto& s = SensorHub::instance().sensors();
  if (i < 0 || static_cast<std::size_t>(i) >= s.size())
    return nullptr;
  return &s[static_cast<std::size_t>(i)];
}

jint nCount(JNIEnv*, jclass) {
  return static_cast<jint>(SensorHub::instance().sensors().size());
}
jint nHandle(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->handle : 0;
}
jint nType(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->type : 0;
}
jint nVersion(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->version : 1;
}
jint nMinDelayUs(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->min_delay_us : 0;
}
jint nValueCount(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->value_count : 3;
}
jfloat nMaxRange(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->max_range : 0.0f;
}
jfloat nResolution(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->resolution : 0.0f;
}
jfloat nPower(JNIEnv*, jclass, jint i) {
  const auto* d = desc_at(i);
  return d ? d->power : 0.0f;
}
jstring nName(JNIEnv* e, jclass, jint i) {
  const auto* d = desc_at(i);
  return e->NewStringUTF(d ? d->name.c_str() : "");
}
jstring nVendor(JNIEnv* e, jclass, jint i) {
  const auto* d = desc_at(i);
  return e->NewStringUTF(d ? d->vendor.c_str() : "");
}
jboolean nEnable(JNIEnv*, jclass, jint handle, jint periodUs) {
  return SensorHub::instance().enable(handle, periodUs) ? JNI_TRUE : JNI_FALSE;
}
void nDisable(JNIEnv*, jclass, jint handle) {
  SensorHub::instance().disable(handle);
}

// Hub listener (runs on the hub's poll thread): push one sample up to Java. The
// poll thread attaches to the JVM as a daemon once (it lives for the process),
// then stays attached.
void deliver(const SensorSample& s) {
  if (g_vm == nullptr || g_cls == nullptr || g_on_event == nullptr)
    return;
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env),
                                          nullptr) != JNI_OK)
      return;
  }
  const int n = s.value_count > 0 ? s.value_count : 0;
  jfloatArray vals = env->NewFloatArray(n);
  if (vals == nullptr) {
    env->ExceptionClear();
    return;
  }
  env->SetFloatArrayRegion(vals, 0, n, s.values);
  env->CallStaticVoidMethod(g_cls, g_on_event, s.handle, vals, s.accuracy,
                            static_cast<jlong>(s.timestamp_ns));
  if (env->ExceptionCheck())
    env->ExceptionClear();
  env->DeleteLocalRef(vals);
}

const JNINativeMethod kMethods[] = {
    {"nCount", "()I", reinterpret_cast<void*>(nCount)},
    {"nHandle", "(I)I", reinterpret_cast<void*>(nHandle)},
    {"nType", "(I)I", reinterpret_cast<void*>(nType)},
    {"nVersion", "(I)I", reinterpret_cast<void*>(nVersion)},
    {"nMinDelayUs", "(I)I", reinterpret_cast<void*>(nMinDelayUs)},
    {"nValueCount", "(I)I", reinterpret_cast<void*>(nValueCount)},
    {"nMaxRange", "(I)F", reinterpret_cast<void*>(nMaxRange)},
    {"nResolution", "(I)F", reinterpret_cast<void*>(nResolution)},
    {"nPower", "(I)F", reinterpret_cast<void*>(nPower)},
    {"nName", "(I)Ljava/lang/String;", reinterpret_cast<void*>(nName)},
    {"nVendor", "(I)Ljava/lang/String;", reinterpret_cast<void*>(nVendor)},
    {"nEnable", "(II)Z", reinterpret_cast<void*>(nEnable)},
    {"nDisable", "(I)V", reinterpret_cast<void*>(nDisable)},
};
}  // namespace

jint register_sensor_natives(JNIEnv* env) {
  env->GetJavaVM(&g_vm);
  jclass c = env->FindClass("org/acl/internal/AclSensorManager");
  if (c == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "AclSensorManager not on classpath");
    return JNI_ERR;
  }
  g_cls = static_cast<jclass>(env->NewGlobalRef(c));
  g_on_event = env->GetStaticMethodID(g_cls, "onSensorEvent", "(I[FIJ)V");
  if (g_on_event == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "AclSensorManager.onSensorEvent missing");
    return JNI_ERR;
  }
  const jint rc = env->RegisterNatives(
      g_cls, kMethods,
      static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
  env->DeleteLocalRef(c);
  if (rc != JNI_OK) {
    log::w(kTag, "RegisterNatives failed (rc={})", rc);
    return rc;
  }
  SensorHub::instance().set_listener(deliver);
  log::i(kTag, "SensorManager natives registered ({} sensor(s))",
         SensorHub::instance().sensors().size());
  return JNI_OK;
}

}  // namespace acl::svc::sensor

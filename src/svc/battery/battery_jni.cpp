// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.BatteryBridge (clean-room)
// to the svc/battery host power backend (UPower → ACTION_BATTERY_CHANGED). No GPL-3.0 sources.
#include "svc/battery/battery_jni.h"

#include <memory>
#include <string_view>

#include "log/log.h"
#include "svc/battery/battery_backend.h"

namespace acl::battery {
namespace {

constexpr std::string_view kTag = "svc.battery";

// The process-wide battery backend, created on first native registration and kept
// alive for the VM lifetime (owns the D-Bus connection + async event loop).
std::unique_ptr<IBatteryBackend>& backend() {
  static std::unique_ptr<IBatteryBackend> b;
  return b;
}

// BatteryBridge.nSnapshot(): the ACTION_BATTERY_CHANGED int extras as one int[], in
// the order the Java bridge unpacks: [level, scale, temperature(dC), voltage(mV),
// status, health, plugged, present(0/1)].
jintArray JNICALL n_snapshot(JNIEnv* env, jclass) {
  AndroidBatteryState s;
  if (const IBatteryBackend* b = backend().get()) s = b->snapshot();
  jint vals[8] = {s.level,  s.scale,   s.temperature_dc, s.voltage_mv,
                  s.status, s.health,  s.plugged,        s.present ? 1 : 0};
  jintArray arr = env->NewIntArray(8);
  if (arr != nullptr) env->SetIntArrayRegion(arr, 0, 8, vals);
  return arr;
}

// BatteryBridge.nTechnology(): EXTRA_TECHNOLOGY string ("Li-ion", …).
jstring JNICALL n_technology(JNIEnv* env, jclass) {
  AndroidBatteryState s;
  if (const IBatteryBackend* b = backend().get()) s = b->snapshot();
  return env->NewStringUTF(s.technology.c_str());
}

}  // namespace

void register_battery_natives(JNIEnv* env) {
  if (!backend()) backend() = make_battery_backend();

  jclass cls = env->FindClass("org/acl/internal/BatteryBridge");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "org.acl.internal.BatteryBridge not on the boot classpath; "
                 "battery natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nSnapshot"), const_cast<char*>("()[I"),
       reinterpret_cast<void*>(&n_snapshot)},
      {const_cast<char*>("nTechnology"), const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_technology)},
  };
  if (env->RegisterNatives(cls, m, 2) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(BatteryBridge) failed");
  } else {
    log::i(kTag, "battery natives registered (backend: {})", backend()->name());
  }
  env->DeleteLocalRef(cls);
}

}  // namespace acl::battery

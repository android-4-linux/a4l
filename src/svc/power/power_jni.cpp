// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.PowerBridge (clean-room)
// to the svc/power host backend (systemd-logind session state + inhibitor wake
// locks, power-profiles battery-saver, kernel thermal zones →
// android.os.PowerManager / IThermalService). No GPL-3.0 sources.
#include "svc/power/power_jni.h"

#include <memory>
#include <string>
#include <string_view>

#include "log/log.h"
#include "svc/power/power_backend.h"

namespace acl::power {
namespace {

constexpr std::string_view kTag = "svc.power";

// The process-wide power backend, created on first native registration and kept
// alive for the VM lifetime (owns the D-Bus connection + async event loop, and
// the inhibitor fds held for live wake locks).
std::unique_ptr<IPowerBackend>& backend() {
  static std::unique_ptr<IPowerBackend> b;
  return b;
}

// PowerBridge.nSnapshot(): the power state as one int[], in the order the Java
// bridge unpacks: [interactive(0/1), powerSave(0/1), deviceIdle(0/1),
// thermalStatus].
jintArray JNICALL n_snapshot(JNIEnv* env, jclass) {
  AndroidPowerState s;
  if (const IPowerBackend* b = backend().get())
    s = b->snapshot();
  jint vals[4] = {s.interactive ? 1 : 0, s.power_save ? 1 : 0,
                  s.device_idle ? 1 : 0, s.thermal_status};
  jintArray arr = env->NewIntArray(4);
  if (arr != nullptr)
    env->SetIntArrayRegion(arr, 0, 4, vals);
  return arr;
}

// PowerBridge.nAcquireWakeLock(flags, tag): take a host inhibitor; returns the
// cookie to release with (0 when unbacked — the Java side still treats the lock
// as held).
jlong JNICALL n_acquire_wake_lock(JNIEnv* env,
                                  jclass,
                                  jint flags,
                                  jstring tag) {
  std::string t;
  if (tag != nullptr) {
    if (const char* c = env->GetStringUTFChars(tag, nullptr)) {
      t.assign(c);
      env->ReleaseStringUTFChars(tag, c);
    }
  }
  IPowerBackend* b = backend().get();
  return b != nullptr ? static_cast<jlong>(b->acquire_wake_lock(flags, t)) : 0;
}

// PowerBridge.nReleaseWakeLock(cookie): drop the inhibitor taken for that
// cookie.
void JNICALL n_release_wake_lock(JNIEnv*, jclass, jlong cookie) {
  if (IPowerBackend* b = backend().get())
    b->release_wake_lock(static_cast<long>(cookie));
}

}  // namespace

void register_power_natives(JNIEnv* env) {
  if (!backend())
    backend() = make_power_backend();

  jclass cls = env->FindClass("org/acl/internal/PowerBridge");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag,
           "org.acl.internal.PowerBridge not on the boot classpath; "
           "power natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nSnapshot"), const_cast<char*>("()[I"),
       reinterpret_cast<void*>(&n_snapshot)},
      {const_cast<char*>("nAcquireWakeLock"),
       const_cast<char*>("(ILjava/lang/String;)J"),
       reinterpret_cast<void*>(&n_acquire_wake_lock)},
      {const_cast<char*>("nReleaseWakeLock"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&n_release_wake_lock)},
  };
  if (env->RegisterNatives(cls, m, 3) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(PowerBridge) failed");
  } else {
    const AndroidPowerState s = backend()->snapshot();
    log::i(kTag,
           "power natives registered (backend: {}; interactive={} "
           "power-save={} idle={} thermal={})",
           backend()->name(), s.interactive, s.power_save, s.device_idle,
           s.thermal_status);
  }
  env->DeleteLocalRef(cls);
}

}  // namespace acl::power

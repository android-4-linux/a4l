// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.BluetoothBridge (clean-room)
// to the svc/bluetooth host BlueZ backend (Adapter1/Device1). No GPL-3.0 sources.
#include "svc/bluetooth/bt_jni.h"

#include <memory>
#include <string>
#include <string_view>

#include "log/log.h"
#include "svc/bluetooth/bt_backend.h"

namespace acl::bt {
namespace {

constexpr std::string_view kTag = "svc.bluetooth";

// The process-wide Bluetooth backend, created on first native registration and kept
// alive for the VM lifetime (owns the D-Bus connection + async event loop + cache).
std::unique_ptr<IBtBackend>& backend() {
  static std::unique_ptr<IBtBackend> b;
  return b;
}

// Look up one bonded device by address in the current snapshot.
const BtDevice* find_device(const AndroidBtState& s, JNIEnv* env, jstring jaddr) {
  if (jaddr == nullptr) return nullptr;
  const char* c = env->GetStringUTFChars(jaddr, nullptr);
  const std::string addr(c != nullptr ? c : "");
  if (c != nullptr) env->ReleaseStringUTFChars(jaddr, c);
  for (const BtDevice& d : s.bonded)
    if (d.address == addr) return &d;
  return nullptr;
}

// BluetoothAdapter.getState() (STATE_OFF when no adapter/backend).
jint JNICALL n_adapter_state(JNIEnv*, jclass) {
  const IBtBackend* b = backend().get();
  return b != nullptr ? b->snapshot().state : kStateOff;
}
jstring JNICALL n_address(JNIEnv* env, jclass) {
  const IBtBackend* b = backend().get();
  return env->NewStringUTF(b != nullptr ? b->snapshot().address.c_str() : "");
}
jstring JNICALL n_name(JNIEnv* env, jclass) {
  const IBtBackend* b = backend().get();
  return env->NewStringUTF(b != nullptr ? b->snapshot().name.c_str() : "");
}
jboolean JNICALL n_is_discovering(JNIEnv*, jclass) {
  const IBtBackend* b = backend().get();
  return (b != nullptr && b->snapshot().discovering) ? JNI_TRUE : JNI_FALSE;
}
// BluetoothAdapter.getBondedDevices() → the paired device addresses.
jobjectArray JNICALL n_bonded_addresses(JNIEnv* env, jclass) {
  const IBtBackend* b = backend().get();
  const AndroidBtState s = b != nullptr ? b->snapshot() : AndroidBtState{};
  jclass str_cls = env->FindClass("java/lang/String");
  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(s.bonded.size()), str_cls, nullptr);
  if (arr != nullptr)
    for (jsize i = 0; i < static_cast<jsize>(s.bonded.size()); ++i)
      env->SetObjectArrayElement(arr, i, env->NewStringUTF(s.bonded[i].address.c_str()));
  env->DeleteLocalRef(str_cls);
  return arr;
}
jstring JNICALL n_device_name(JNIEnv* env, jclass, jstring addr) {
  const IBtBackend* b = backend().get();
  const AndroidBtState s = b != nullptr ? b->snapshot() : AndroidBtState{};
  const BtDevice* d = find_device(s, env, addr);
  return env->NewStringUTF(d != nullptr ? d->name.c_str() : "");
}
jint JNICALL n_device_class(JNIEnv* env, jclass, jstring addr) {
  const IBtBackend* b = backend().get();
  const AndroidBtState s = b != nullptr ? b->snapshot() : AndroidBtState{};
  const BtDevice* d = find_device(s, env, addr);
  return d != nullptr ? d->cls : 0;
}
jint JNICALL n_device_bond_state(JNIEnv* env, jclass, jstring addr) {
  const IBtBackend* b = backend().get();
  const AndroidBtState s = b != nullptr ? b->snapshot() : AndroidBtState{};
  const BtDevice* d = find_device(s, env, addr);
  return d != nullptr ? d->bond_state : kBondNone;
}
jboolean JNICALL n_set_powered(JNIEnv*, jclass, jboolean on) {
  IBtBackend* b = backend().get();
  return (b != nullptr && b->set_powered(on == JNI_TRUE)) ? JNI_TRUE : JNI_FALSE;
}
jboolean JNICALL n_start_discovery(JNIEnv*, jclass) {
  IBtBackend* b = backend().get();
  return (b != nullptr && b->start_discovery()) ? JNI_TRUE : JNI_FALSE;
}
jboolean JNICALL n_stop_discovery(JNIEnv*, jclass) {
  IBtBackend* b = backend().get();
  return (b != nullptr && b->stop_discovery()) ? JNI_TRUE : JNI_FALSE;
}

}  // namespace

void register_bt_natives(JNIEnv* env) {
  if (!backend()) backend() = make_bt_backend();

  jclass cls = env->FindClass("org/acl/internal/BluetoothBridge");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "org.acl.internal.BluetoothBridge not on the boot classpath; "
                 "bluetooth natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nAdapterState"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&n_adapter_state)},
      {const_cast<char*>("nAddress"), const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_address)},
      {const_cast<char*>("nName"), const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_name)},
      {const_cast<char*>("nIsDiscovering"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&n_is_discovering)},
      {const_cast<char*>("nBondedAddresses"), const_cast<char*>("()[Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_bonded_addresses)},
      {const_cast<char*>("nDeviceName"), const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_device_name)},
      {const_cast<char*>("nDeviceClass"), const_cast<char*>("(Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&n_device_class)},
      {const_cast<char*>("nDeviceBondState"), const_cast<char*>("(Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&n_device_bond_state)},
      {const_cast<char*>("nSetPowered"), const_cast<char*>("(Z)Z"),
       reinterpret_cast<void*>(&n_set_powered)},
      {const_cast<char*>("nStartDiscovery"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&n_start_discovery)},
      {const_cast<char*>("nStopDiscovery"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&n_stop_discovery)},
  };
  if (env->RegisterNatives(cls, m, sizeof(m) / sizeof(m[0])) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(BluetoothBridge) failed");
  } else {
    log::i(kTag, "bluetooth natives registered (backend: {})", backend()->name());
  }
  env->DeleteLocalRef(cls);
}

}  // namespace acl::bt

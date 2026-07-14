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

// For the async change sink: the backend fires it on the D-Bus event-loop thread, so we attach
// that thread to the JVM (as a daemon, once) and ping BluetoothBridge.onBluetoothChanged(),
// which hops to the app main thread to diff + deliver the ACTION_* broadcasts (P2).
JavaVM* g_vm = nullptr;
jclass g_bridge_cls = nullptr;      // global ref
jmethodID g_on_changed = nullptr;   // static void onBluetoothChanged()

void bt_sink(const AndroidBtState&) {
  if (g_vm == nullptr || g_bridge_cls == nullptr || g_on_changed == nullptr) return;
  JNIEnv* e = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&e), nullptr) != JNI_OK) return;
  }
  e->CallStaticVoidMethod(g_bridge_cls, g_on_changed);
  if (e->ExceptionCheck() == JNI_TRUE) e->ExceptionClear();
}

// Look up one bonded device by address in the current snapshot.
const BtDevice* find_device(const AndroidBtState& s, JNIEnv* env, jstring jaddr) {
  if (jaddr == nullptr) return nullptr;
  const char* c = env->GetStringUTFChars(jaddr, nullptr);
  const std::string addr(c != nullptr ? c : "");
  if (c != nullptr) env->ReleaseStringUTFChars(jaddr, c);
  for (const BtDevice& d : s.devices)
    if (d.address == addr) return &d;
  return nullptr;
}

// Build a String[] of the addresses in `s` selected by `pred(device)`.
template <class Pred>
jobjectArray addresses(JNIEnv* env, const AndroidBtState& s, Pred pred) {
  std::vector<const BtDevice*> sel;
  for (const BtDevice& d : s.devices)
    if (pred(d)) sel.push_back(&d);
  jclass str_cls = env->FindClass("java/lang/String");
  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(sel.size()), str_cls, nullptr);
  if (arr != nullptr)
    for (jsize i = 0; i < static_cast<jsize>(sel.size()); ++i)
      env->SetObjectArrayElement(arr, i, env->NewStringUTF(sel[i]->address.c_str()));
  env->DeleteLocalRef(str_cls);
  return arr;
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
  return addresses(env, s, [](const BtDevice& d) { return d.bond_state == kBondBonded; });
}
// All known device addresses (paired + discovered) — for ACTION_FOUND diffing (P2).
jobjectArray JNICALL n_all_devices(JNIEnv* env, jclass) {
  const IBtBackend* b = backend().get();
  const AndroidBtState s = b != nullptr ? b->snapshot() : AndroidBtState{};
  return addresses(env, s, [](const BtDevice&) { return true; });
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
      {const_cast<char*>("nAllDevices"), const_cast<char*>("()[Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_all_devices)},
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
    env->DeleteLocalRef(cls);
    return;
  }
  log::i(kTag, "bluetooth natives registered (backend: {})", backend()->name());

  // Arm the async change sink → BluetoothBridge.onBluetoothChanged() (P2 broadcasts).
  env->GetJavaVM(&g_vm);
  g_bridge_cls = static_cast<jclass>(env->NewGlobalRef(cls));
  g_on_changed = env->GetStaticMethodID(cls, "onBluetoothChanged", "()V");
  if (g_on_changed != nullptr && backend()) backend()->set_sink(&bt_sink);
  else env->ExceptionClear();
  env->DeleteLocalRef(cls);
}

}  // namespace acl::bt

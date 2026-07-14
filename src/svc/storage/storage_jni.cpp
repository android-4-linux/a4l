// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.StorageBridge (clean-room) to the
// svc/storage host UDisks2 backend (Block/Filesystem/Drive). No GPL-3.0 sources.
#include "svc/storage/storage_jni.h"

#include <memory>
#include <string>
#include <string_view>

#include "log/log.h"
#include "svc/storage/storage_backend.h"

namespace acl::storage {
namespace {

constexpr std::string_view kTag = "svc.storage";

// The process-wide storage backend, created on first native registration and kept alive for
// the VM lifetime (owns the D-Bus connection + async event loop + cache).
std::unique_ptr<IStorageBackend>& backend() {
  static std::unique_ptr<IStorageBackend> b;
  return b;
}

// For the async change sink: the backend fires it on the D-Bus event-loop thread, so we attach
// that thread to the JVM (as a daemon, once) and ping StorageBridge.onStorageChanged(), which
// hops to the app main thread to diff + deliver the ACTION_MEDIA_* broadcasts.
JavaVM* g_vm = nullptr;
jclass g_bridge_cls = nullptr;     // global ref
jmethodID g_on_changed = nullptr;  // static void onStorageChanged()

void storage_sink(const AndroidStorageState&) {
  if (g_vm == nullptr || g_bridge_cls == nullptr || g_on_changed == nullptr) return;
  JNIEnv* e = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&e), nullptr) != JNI_OK) return;
  }
  e->CallStaticVoidMethod(g_bridge_cls, g_on_changed);
  if (e->ExceptionCheck() == JNI_TRUE) e->ExceptionClear();
}

// Look up the i-th removable volume in the current snapshot (bounds-checked → nullptr).
const StorageVol* vol_at(const AndroidStorageState& s, jint i) {
  if (i < 0 || static_cast<std::size_t>(i) >= s.volumes.size()) return nullptr;
  return &s.volumes[static_cast<std::size_t>(i)];
}

jint JNICALL n_volume_count(JNIEnv*, jclass) {
  const IStorageBackend* b = backend().get();
  return b != nullptr ? static_cast<jint>(b->snapshot().volumes.size()) : 0;
}
jstring JNICALL n_volume_id(JNIEnv* env, jclass, jint i) {
  const IStorageBackend* b = backend().get();
  const AndroidStorageState s = b != nullptr ? b->snapshot() : AndroidStorageState{};
  const StorageVol* v = vol_at(s, i);
  return env->NewStringUTF(v != nullptr ? v->id.c_str() : "");
}
jstring JNICALL n_volume_path(JNIEnv* env, jclass, jint i) {
  const IStorageBackend* b = backend().get();
  const AndroidStorageState s = b != nullptr ? b->snapshot() : AndroidStorageState{};
  const StorageVol* v = vol_at(s, i);
  return env->NewStringUTF(v != nullptr ? v->path.c_str() : "");
}
jstring JNICALL n_volume_description(JNIEnv* env, jclass, jint i) {
  const IStorageBackend* b = backend().get();
  const AndroidStorageState s = b != nullptr ? b->snapshot() : AndroidStorageState{};
  const StorageVol* v = vol_at(s, i);
  return env->NewStringUTF(v != nullptr ? v->description.c_str() : "");
}
jstring JNICALL n_volume_fs_uuid(JNIEnv* env, jclass, jint i) {
  const IStorageBackend* b = backend().get();
  const AndroidStorageState s = b != nullptr ? b->snapshot() : AndroidStorageState{};
  const StorageVol* v = vol_at(s, i);
  return env->NewStringUTF(v != nullptr ? v->fs_uuid.c_str() : "");
}
jstring JNICALL n_volume_state(JNIEnv* env, jclass, jint i) {
  const IStorageBackend* b = backend().get();
  const AndroidStorageState s = b != nullptr ? b->snapshot() : AndroidStorageState{};
  const StorageVol* v = vol_at(s, i);
  return env->NewStringUTF(v != nullptr ? v->state.c_str() : "unmounted");
}

}  // namespace

void register_storage_natives(JNIEnv* env) {
  if (!backend()) backend() = make_storage_backend();

  jclass cls = env->FindClass("org/acl/internal/StorageBridge");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "org.acl.internal.StorageBridge not on the boot classpath; "
                 "storage natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nVolumeCount"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&n_volume_count)},
      {const_cast<char*>("nVolumeId"), const_cast<char*>("(I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_volume_id)},
      {const_cast<char*>("nVolumePath"), const_cast<char*>("(I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_volume_path)},
      {const_cast<char*>("nVolumeDescription"), const_cast<char*>("(I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_volume_description)},
      {const_cast<char*>("nVolumeFsUuid"), const_cast<char*>("(I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_volume_fs_uuid)},
      {const_cast<char*>("nVolumeState"), const_cast<char*>("(I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_volume_state)},
  };
  if (env->RegisterNatives(cls, m, sizeof(m) / sizeof(m[0])) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(StorageBridge) failed");
    env->DeleteLocalRef(cls);
    return;
  }
  log::i(kTag, "storage natives registered (backend: {})", backend()->name());

  // Arm the async change sink → StorageBridge.onStorageChanged() (media mount/unmount).
  env->GetJavaVM(&g_vm);
  g_bridge_cls = static_cast<jclass>(env->NewGlobalRef(cls));
  g_on_changed = env->GetStaticMethodID(cls, "onStorageChanged", "()V");
  if (g_on_changed != nullptr && backend()) backend()->set_sink(&storage_sink);
  else env->ExceptionClear();
  env->DeleteLocalRef(cls);
}

}  // namespace acl::storage

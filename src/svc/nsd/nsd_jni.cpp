// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.NsdBridge (clean-room) to the
// svc/nsd host discovery backend (Avahi -> android.net.nsd.NsdManager). Async
// delivery pattern mirrors svc/location/location_jni.cpp. No GPL-3.0 sources.
#include "svc/nsd/nsd_jni.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "log/log.h"
#include "svc/nsd/nsd_backend.h"

namespace acl::nsd {
namespace {

constexpr std::string_view kTag = "svc.nsd";

JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;            // global ref to NsdBridge
jmethodID g_on_found = nullptr;    // static onServiceFound(JLjava/lang/String;...)V
jmethodID g_on_lost = nullptr;     // static onServiceLost(...)V
jmethodID g_on_resolved = nullptr; // static onResolved(...)V

// The process-wide discovery backend (owns the D-Bus connection + async event loop).
std::unique_ptr<INsdBackend>& backend() {
  static std::unique_ptr<INsdBackend> b;
  return b;
}

// The Avahi event-loop thread attaches to the JVM once (it lives for the process).
JNIEnv* event_env() {
  if (g_vm == nullptr) return nullptr;
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK)
      return nullptr;
  }
  return env;
}

std::string to_utf8(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  if (c == nullptr) return {};
  std::string out(c);
  env->ReleaseStringUTFChars(s, c);
  return out;
}

// TXT records travel as a flat String[] of alternating key, value.
jobjectArray txt_to_array(JNIEnv* env,
                          const std::vector<std::pair<std::string, std::string>>& txt) {
  jclass str_cls = env->FindClass("java/lang/String");
  if (str_cls == nullptr) { env->ExceptionClear(); return nullptr; }
  jobjectArray arr =
      env->NewObjectArray(static_cast<jsize>(txt.size() * 2), str_cls, nullptr);
  if (arr != nullptr) {
    jsize i = 0;
    for (const auto& [k, v] : txt) {
      jstring jk = env->NewStringUTF(k.c_str());
      jstring jv = env->NewStringUTF(v.c_str());
      env->SetObjectArrayElement(arr, i++, jk);
      env->SetObjectArrayElement(arr, i++, jv);
      if (jk != nullptr) env->DeleteLocalRef(jk);
      if (jv != nullptr) env->DeleteLocalRef(jv);
    }
  }
  env->DeleteLocalRef(str_cls);
  return arr;
}

// ── sinks: backend (Avahi loop thread) -> Java ────────────────────────────────
void on_discovery(long handle, DiscoveryEvent ev, const NsdService& s) {
  JNIEnv* env = event_env();
  if (env == nullptr || g_cls == nullptr) return;
  jmethodID m = (ev == DiscoveryEvent::kFound) ? g_on_found : g_on_lost;
  if (m == nullptr) return;
  jstring n = env->NewStringUTF(s.name.c_str());
  jstring t = env->NewStringUTF(s.type.c_str());
  jstring d = env->NewStringUTF(s.domain.c_str());
  env->CallStaticVoidMethod(g_cls, m, static_cast<jlong>(handle), n, t, d);
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (n != nullptr) env->DeleteLocalRef(n);
  if (t != nullptr) env->DeleteLocalRef(t);
  if (d != nullptr) env->DeleteLocalRef(d);
}

void on_resolve(bool ok, const NsdService& s) {
  JNIEnv* env = event_env();
  if (env == nullptr || g_cls == nullptr || g_on_resolved == nullptr) return;
  jstring n = env->NewStringUTF(s.name.c_str());
  jstring t = env->NewStringUTF(s.type.c_str());
  jstring d = env->NewStringUTF(s.domain.c_str());
  jstring h = env->NewStringUTF(s.host.c_str());
  jstring a = env->NewStringUTF(s.address.c_str());
  jobjectArray txt = txt_to_array(env, s.txt);
  env->CallStaticVoidMethod(g_cls, g_on_resolved, ok ? JNI_TRUE : JNI_FALSE, n, t, d, h, a,
                            static_cast<jint>(s.port), txt);
  if (env->ExceptionCheck()) env->ExceptionClear();
  for (jobject o : {static_cast<jobject>(n), static_cast<jobject>(t), static_cast<jobject>(d),
                    static_cast<jobject>(h), static_cast<jobject>(a),
                    static_cast<jobject>(txt)}) {
    if (o != nullptr) env->DeleteLocalRef(o);
  }
}

// ── natives ───────────────────────────────────────────────────────────────────
jboolean JNICALL n_available(JNIEnv*, jclass) {
  const INsdBackend* b = backend().get();
  return (b != nullptr && b->available()) ? JNI_TRUE : JNI_FALSE;
}

jlong JNICALL n_start_discovery(JNIEnv* env, jclass, jstring type) {
  INsdBackend* b = backend().get();
  if (b == nullptr) return 0;
  return static_cast<jlong>(b->start_discovery(to_utf8(env, type)));
}

void JNICALL n_stop_discovery(JNIEnv*, jclass, jlong handle) {
  if (INsdBackend* b = backend().get()) b->stop_discovery(static_cast<long>(handle));
}

jboolean JNICALL n_resolve(JNIEnv* env, jclass, jstring name, jstring type, jstring domain) {
  INsdBackend* b = backend().get();
  if (b == nullptr) return JNI_FALSE;
  return b->resolve(to_utf8(env, name), to_utf8(env, type), to_utf8(env, domain)) ? JNI_TRUE
                                                                                  : JNI_FALSE;
}

jlong JNICALL n_register(JNIEnv* env, jclass, jstring name, jstring type, jstring domain,
                         jint port, jobjectArray txt) {
  INsdBackend* b = backend().get();
  if (b == nullptr) return 0;
  NsdService s;
  s.name = to_utf8(env, name);
  s.type = to_utf8(env, type);
  s.domain = to_utf8(env, domain);
  s.port = static_cast<int>(port);
  if (txt != nullptr) {
    const jsize n = env->GetArrayLength(txt);
    for (jsize i = 0; i + 1 < n; i += 2) {
      auto k = static_cast<jstring>(env->GetObjectArrayElement(txt, i));
      auto v = static_cast<jstring>(env->GetObjectArrayElement(txt, i + 1));
      s.txt.emplace_back(to_utf8(env, k), to_utf8(env, v));
      if (k != nullptr) env->DeleteLocalRef(k);
      if (v != nullptr) env->DeleteLocalRef(v);
    }
  }
  return static_cast<jlong>(b->register_service(s));
}

void JNICALL n_unregister(JNIEnv*, jclass, jlong handle) {
  if (INsdBackend* b = backend().get()) b->unregister_service(static_cast<long>(handle));
}

}  // namespace

void register_nsd_natives(JNIEnv* env) {
  if (!backend()) backend() = make_nsd_backend();
  env->GetJavaVM(&g_vm);

  jclass c = env->FindClass("org/acl/internal/NsdBridge");
  if (c == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "org.acl.internal.NsdBridge not on the boot classpath; nsd natives not registered");
    return;
  }
  g_cls = static_cast<jclass>(env->NewGlobalRef(c));
  env->DeleteLocalRef(c);

  g_on_found = env->GetStaticMethodID(
      g_cls, "onServiceFound", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  g_on_lost = env->GetStaticMethodID(
      g_cls, "onServiceLost", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  g_on_resolved = env->GetStaticMethodID(
      g_cls, "onResolved",
      "(ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
      "Ljava/lang/String;I[Ljava/lang/String;)V");
  if (env->ExceptionCheck()) env->ExceptionClear();

  const JNINativeMethod m[] = {
      {const_cast<char*>("nAvailable"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&n_available)},
      {const_cast<char*>("nStartDiscovery"), const_cast<char*>("(Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&n_start_discovery)},
      {const_cast<char*>("nStopDiscovery"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&n_stop_discovery)},
      {const_cast<char*>("nResolve"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z"),
       reinterpret_cast<void*>(&n_resolve)},
      {const_cast<char*>("nRegister"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I"
                         "[Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&n_register)},
      {const_cast<char*>("nUnregister"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&n_unregister)},
  };
  if (env->RegisterNatives(g_cls, m, 6) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(NsdBridge) failed");
    return;
  }
  backend()->set_sinks(on_discovery, on_resolve);
  log::i(kTag, "nsd natives registered (backend: {}; available={})", backend()->name(),
         backend()->available());
}

}  // namespace acl::nsd

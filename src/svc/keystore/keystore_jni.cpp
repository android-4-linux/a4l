// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam; JNI RegisterNatives pattern per acl svc/* modules. No GPL-3.0 sources.
#include "svc/keystore/keystore_jni.h"

#include "svc/keystore/keystore_backend.h"
#include "log/log.h"

#include <memory>
#include <string>
#include <vector>

namespace acl::keystore {
namespace {
constexpr const char* kTag = "acl.svc.keystore.jni";

// One backend for the process, created on first register. The AndroidKeyStore JCE provider is a
// process singleton too, so a single backend matches its lifetime.
IKeystoreBackend* backend() {
  static std::unique_ptr<IKeystoreBackend> be = make_keystore_backend();
  return be.get();
}

std::string jstr(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c != nullptr ? c : "";
  if (c != nullptr) env->ReleaseStringUTFChars(s, c);
  return out;
}

jboolean Keystore_nStore(JNIEnv* env, jclass, jstring pkg, jstring alias, jbyteArray blob) {
  if (blob == nullptr) return JNI_FALSE;
  const jsize n = env->GetArrayLength(blob);
  std::vector<std::uint8_t> data(static_cast<size_t>(n));
  if (n > 0) env->GetByteArrayRegion(blob, 0, n, reinterpret_cast<jbyte*>(data.data()));
  return backend()->store(jstr(env, pkg), jstr(env, alias), data) ? JNI_TRUE : JNI_FALSE;
}

jbyteArray Keystore_nLoad(JNIEnv* env, jclass, jstring pkg, jstring alias) {
  std::vector<std::uint8_t> out;
  if (!backend()->load(jstr(env, pkg), jstr(env, alias), out)) return nullptr;  // absent
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(out.size()));
  if (arr != nullptr && !out.empty())
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(out.size()),
                            reinterpret_cast<const jbyte*>(out.data()));
  return arr;
}

jboolean Keystore_nContains(JNIEnv* env, jclass, jstring pkg, jstring alias) {
  return backend()->contains(jstr(env, pkg), jstr(env, alias)) ? JNI_TRUE : JNI_FALSE;
}

jboolean Keystore_nErase(JNIEnv* env, jclass, jstring pkg, jstring alias) {
  return backend()->erase(jstr(env, pkg), jstr(env, alias)) ? JNI_TRUE : JNI_FALSE;
}

jobjectArray Keystore_nAliases(JNIEnv* env, jclass, jstring pkg) {
  const std::vector<std::string> names = backend()->aliases(jstr(env, pkg));
  jclass strCls = env->FindClass("java/lang/String");
  if (strCls == nullptr) { env->ExceptionClear(); return nullptr; }
  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(names.size()), strCls, nullptr);
  if (arr == nullptr) { env->ExceptionClear(); return nullptr; }
  for (jsize i = 0; i < static_cast<jsize>(names.size()); ++i) {
    jstring s = env->NewStringUTF(names[i].c_str());
    env->SetObjectArrayElement(arr, i, s);
    env->DeleteLocalRef(s);
  }
  env->DeleteLocalRef(strCls);
  return arr;
}

}  // namespace

void register_keystore_natives(JNIEnv* env) {
  jclass c = env->FindClass("org/acl/internal/KeystoreBridge");
  if (c == nullptr) { env->ExceptionClear(); return; }  // provider jar not on the classpath
  const JNINativeMethod m[] = {
      {const_cast<char*>("nStore"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;[B)Z"),
       reinterpret_cast<void*>(Keystore_nStore)},
      {const_cast<char*>("nLoad"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)[B"),
       reinterpret_cast<void*>(Keystore_nLoad)},
      {const_cast<char*>("nContains"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Z"),
       reinterpret_cast<void*>(Keystore_nContains)},
      {const_cast<char*>("nErase"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Z"),
       reinterpret_cast<void*>(Keystore_nErase)},
      {const_cast<char*>("nAliases"),
       const_cast<char*>("(Ljava/lang/String;)[Ljava/lang/String;"),
       reinterpret_cast<void*>(Keystore_nAliases)},
  };
  if (env->RegisterNatives(c, m, 5) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(KeystoreBridge) failed");
  } else {
    log::i(kTag, "KeystoreBridge natives registered (backend: {})", backend()->name());
  }
  env->DeleteLocalRef(c);
}

}  // namespace acl::keystore

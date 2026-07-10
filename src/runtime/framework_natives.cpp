// SPDX-License-Identifier: Apache-2.0
// Provenance: docs/framework-bootstrap.md §A3; JNI Functions docs (RegisterNatives,
// GetStringUTFChars). android.util.Log priority model from Android SDK docs (also
// mirrored by aclcxx::log::Level). No GPL-3.0 sources.
//
// The JNI "native underside": aclcxx C++ implementations of android.* native
// methods, registered against the vendored framework classes on the boot
// classpath. This is the seam by which the Java framework reaches the aclcxx
// native core (svc/, gfx/, wm/, log/). G2.b wires the first one: android.util.Log
// -> aclcxx::log.

#include "runtime/framework_natives.h"

#include <array>
#include <string>

#include "log/log.h"

namespace aclcxx::runtime {
namespace {

std::string to_utf8(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out(chars != nullptr ? chars : "");
  if (chars != nullptr) env->ReleaseStringUTFChars(s, chars);
  return out;
}

// android.util.Log.write0(priority, tag, msg) -> aclcxx::log::write. Priorities
// are identical between android.util.Log and aclcxx::log::Level (2..7).
jint JNICALL Log_write0(JNIEnv* env, jclass /*cls*/, jint priority, jstring tag,
                        jstring msg) {
  const std::string tag_s = to_utf8(env, tag);
  const std::string msg_s = to_utf8(env, msg);
  int p = priority;
  if (p < static_cast<int>(log::Level::Verbose)) p = static_cast<int>(log::Level::Verbose);
  if (p > static_cast<int>(log::Level::Fatal)) p = static_cast<int>(log::Level::Fatal);
  log::write(static_cast<log::Level>(p), tag_s, msg_s);
  return static_cast<jint>(msg_s.size());
}

}  // namespace

void register_framework_natives(JNIEnv* env) {
  if (env == nullptr) return;

  // android.util.Log — the first vendored framework class with an aclcxx native.
  if (jclass log_cls = env->FindClass("android/util/Log"); log_cls != nullptr) {
    const std::array<JNINativeMethod, 1> methods{{
        {const_cast<char*>("write0"),
         const_cast<char*>("(ILjava/lang/String;Ljava/lang/String;)I"),
         reinterpret_cast<void*>(&Log_write0)},
    }};
    if (env->RegisterNatives(log_cls, methods.data(),
                             static_cast<jint>(methods.size())) != JNI_OK) {
      env->ExceptionClear();
      log::write(log::Level::Warn, "aclcxx.runtime.jvm",
                 "RegisterNatives(android.util.Log) failed");
    } else {
      log::write(log::Level::Info, "aclcxx.runtime.jvm",
                 "framework native bound: android.util.Log");
    }
    env->DeleteLocalRef(log_cls);
  } else {
    env->ExceptionClear();  // no framework on the boot classpath -> nothing to bind
  }
}

}  // namespace aclcxx::runtime

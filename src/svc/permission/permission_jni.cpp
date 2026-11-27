// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.HostPermissions to
// svc/permission (the XDG permission store). No GPL-3.0 sources.
#include "svc/permission/permission_jni.h"

#include <string>
#include <string_view>

#include "log/log.h"
#include "svc/permission/permission_store.h"

namespace acl::permission {
namespace {

constexpr std::string_view kTag = "svc.permission";

std::string utf(JNIEnv* env, jstring j) {
  if (j == nullptr)
    return {};
  const char* c = env->GetStringUTFChars(j, nullptr);
  std::string s(c != nullptr ? c : "");
  if (c != nullptr)
    env->ReleaseStringUTFChars(j, c);
  return s;
}

jboolean JNICALL n_device_allowed(JNIEnv* env,
                                  jclass,
                                  jstring jdevice,
                                  jstring japp) {
  try {
    return device_allowed(utf(env, jdevice), utf(env, japp)) ? JNI_TRUE
                                                             : JNI_FALSE;
  } catch (...) {
    return JNI_FALSE;
  }
}

}  // namespace

void register_permission_natives(JNIEnv* env) {
  jclass cls = env->FindClass("org/acl/internal/HostPermissions");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "org.acl.internal.HostPermissions not on the classpath");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nDeviceAllowed"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Z"),
       reinterpret_cast<void*>(&n_device_allowed)},
  };
  if (env->RegisterNatives(cls, m, 1) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(HostPermissions) failed");
  }
  env->DeleteLocalRef(cls);
}

}  // namespace acl::permission

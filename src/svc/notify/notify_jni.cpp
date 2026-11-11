// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.NotifyBridge to the
// svc/notify host backend (org.freedesktop.Notifications). No GPL-3.0 sources.
#include "svc/notify/notify_jni.h"

#include <memory>
#include <string>
#include <string_view>

#include "log/log.h"
#include "svc/notify/notify_backend.h"

namespace acl::notify {
namespace {

constexpr std::string_view kTag = "svc.notify";

JavaVM* g_vm = nullptr;
jclass g_bridge = nullptr;  // global ref to org.acl.internal.NotifyBridge
jmethodID g_on_event = nullptr;

std::unique_ptr<INotifyBackend>& backend() {
  static std::unique_ptr<INotifyBackend> b;
  return b;
}

// Marshal a Java string (may be null) into std::string.
std::string to_utf8(JNIEnv* env, jstring s) {
  if (s == nullptr)
    return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c)
    env->ReleaseStringUTFChars(s, c);
  return out;
}

// NotifyBridge.nNotify(String app, int replacesId, String summary, String body)
// -> long
jlong JNICALL n_notify(JNIEnv* env,
                       jclass,
                       jstring app,
                       jint replaces_id,
                       jstring summary,
                       jstring body) {
  INotifyBackend* b = backend().get();
  if (b == nullptr)
    return 0;
  return static_cast<jlong>(
      b->notify(to_utf8(env, app), static_cast<uint32_t>(replaces_id),
                to_utf8(env, summary), to_utf8(env, body)));
}

// NotifyBridge.nClose(long id)
void JNICALL n_close(JNIEnv*, jclass, jlong id) {
  if (INotifyBackend* b = backend().get())
    b->close(static_cast<uint32_t>(id));
}

// Dispatch a host action/close event to NotifyBridge.onEvent on the JVM. Runs
// on the D-Bus event-loop thread, so attach it to the VM first.
void dispatch_event(const NotifyEvent& ev) {
  if (g_vm == nullptr || g_bridge == nullptr || g_on_event == nullptr)
    return;
  JNIEnv* env = nullptr;
  if (g_vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) !=
      JNI_OK)
    return;
  jstring action = env->NewStringUTF(ev.action_key.c_str());
  env->CallStaticVoidMethod(
      g_bridge, g_on_event, static_cast<jlong>(ev.id),
      ev.kind == NotifyEvent::Kind::kActionInvoked ? 0 : 1, action,
      static_cast<jint>(ev.reason));
  if (env->ExceptionCheck())
    env->ExceptionClear();
  if (action)
    env->DeleteLocalRef(action);
}

}  // namespace

void register_notify_natives(JNIEnv* env) {
  if (!backend())
    backend() = make_notify_backend();

  jclass cls = env->FindClass("org/acl/internal/NotifyBridge");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag,
           "org.acl.internal.NotifyBridge not on the classpath; "
           "notification natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nNotify"),
       const_cast<char*>(
           "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&n_notify)},
      {const_cast<char*>("nClose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&n_close)},
  };
  if (env->RegisterNatives(cls, m, 2) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(NotifyBridge) failed");
    return;
  }

  // Cache the VM + onEvent(long,int,String,int) for the signal sink.
  env->GetJavaVM(&g_vm);
  g_bridge = static_cast<jclass>(env->NewGlobalRef(cls));
  g_on_event =
      env->GetStaticMethodID(g_bridge, "onEvent", "(JILjava/lang/String;I)V");
  if (g_on_event == nullptr)
    env->ExceptionClear();
  else
    backend()->set_sink(&dispatch_event);

  log::i(kTag, "notification natives registered (backend: {})",
         backend()->name());
  env->DeleteLocalRef(cls);
}

}  // namespace acl::notify

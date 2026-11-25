// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.NotifyBridge to the
// svc/notify host backend (org.freedesktop.Notifications). No GPL-3.0 sources.
#include "svc/notify/notify_jni.h"

#include <cstddef>
#include <cstdint>
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

// Marshal a Java string (may be null) into UTF-8. From the UTF-16 code units,
// not GetStringUTFChars: that gives modified UTF-8, which writes a character
// outside the basic plane (an emoji in a track title) as two surrogates, and
// D-Bus refuses a string that is not valid UTF-8. An unpaired surrogate
// becomes U+FFFD.
std::string to_utf8(JNIEnv* env, jstring s) {
  if (s == nullptr)
    return {};
  const jsize n = env->GetStringLength(s);
  const jchar* c = env->GetStringChars(s, nullptr);
  if (c == nullptr)
    return {};
  std::string out;
  out.reserve(static_cast<std::size_t>(n));
  for (jsize i = 0; i < n; ++i) {
    std::uint32_t cp = c[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && c[i + 1] >= 0xDC00 &&
        c[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (c[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp == 0) {
      // D-Bus strings hold no NUL.
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  env->ReleaseStringChars(s, c);
  return out;
}

// NotifyBridge.nNotify(String app, int replacesId, String summary, String body,
//     String[] actions, byte[] rgba, int w, int h, boolean resident) -> long
// actions is key, label, key, label; rgba is w*h*4 bytes or null.
jlong JNICALL n_notify(JNIEnv* env,
                       jclass,
                       jstring app,
                       jint replaces_id,
                       jstring summary,
                       jstring body,
                       jobjectArray actions,
                       jbyteArray rgba,
                       jint w,
                       jint h,
                       jboolean resident) {
  INotifyBackend* b = backend().get();
  if (b == nullptr)
    return 0;
  NotifyContent c;
  c.app = to_utf8(env, app);
  c.replaces_id = static_cast<uint32_t>(replaces_id);
  c.summary = to_utf8(env, summary);
  c.body = to_utf8(env, body);
  c.resident = resident == JNI_TRUE;
  const jsize n_actions = actions != nullptr ? env->GetArrayLength(actions) : 0;
  for (jsize i = 0; i + 1 < n_actions; i += 2) {
    auto* key = static_cast<jstring>(env->GetObjectArrayElement(actions, i));
    auto* label =
        static_cast<jstring>(env->GetObjectArrayElement(actions, i + 1));
    c.actions.emplace_back(to_utf8(env, key), to_utf8(env, label));
    if (key != nullptr)
      env->DeleteLocalRef(key);
    if (label != nullptr)
      env->DeleteLocalRef(label);
  }
  // The size the caller gives and the array's length have to agree, and the
  // edges stay inside the backend's bound; otherwise no image is sent.
  if (rgba != nullptr && w > 0 && h > 0 && w <= kMaxImageEdge &&
      h <= kMaxImageEdge &&
      static_cast<jlong>(env->GetArrayLength(rgba)) ==
          static_cast<jlong>(w) * h * 4) {
    c.image.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                   4);
    env->GetByteArrayRegion(rgba, 0, static_cast<jsize>(c.image.size()),
                            reinterpret_cast<jbyte*>(c.image.data()));
    c.image_w = w;
    c.image_h = h;
  }
  return static_cast<jlong>(b->notify(c));
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
           "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;"
           "[Ljava/lang/String;[BIIZ)J"),
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

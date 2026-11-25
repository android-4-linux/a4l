// SPDX-License-Identifier: Apache-2.0
// Provenance: original. No GPL-3.0 sources.
//
// JNI seam for org.acl.internal.MprisBridge: the session's state goes down to
// the MPRIS backend and the desktop's commands come back up.
#include "svc/mpris/mpris_jni.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "log/log.h"
#include "svc/mpris/mpris_backend.h"

namespace acl::mpris {
namespace {

constexpr std::string_view kTag = "svc.mpris";

JavaVM* g_vm = nullptr;
jclass g_bridge = nullptr;  // global ref to org.acl.internal.MprisBridge
jmethodID g_on_command = nullptr;

std::unique_ptr<IMprisBackend>& backend() {
  static std::unique_ptr<IMprisBackend> b;
  return b;
}

// D-Bus strings are UTF-8, and GetStringUTFChars gives modified UTF-8, so the
// string is made from its UTF-16 code units. An unpaired surrogate and NUL
// become U+FFFD.
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
    } else if ((cp >= 0xD800 && cp <= 0xDFFF) || cp == 0) {
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

// A bus name element: letters, digits and '_', not starting with a digit.
std::string bus_element(const std::string& package) {
  std::string out;
  out.reserve(package.size() + 1);
  for (const char ch : package) {
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9');
    out.push_back(ok ? ch : '_');
  }
  if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
    out.insert(out.begin(), '_');
  return out;
}

// MprisBridge.nUpdate(String identity, String pkg, int playback, String title,
// String artist, String album, String artUrl, long lengthUs, long positionUs,
// float rate, int can, long track)
jboolean JNICALL n_update(JNIEnv* env,
                          jclass,
                          jstring identity,
                          jstring package,
                          jint playback,
                          jstring title,
                          jstring artist,
                          jstring album,
                          jstring art_url,
                          jlong length_us,
                          jlong position_us,
                          jfloat rate,
                          jint can,
                          jlong track) {
  IMprisBackend* b = backend().get();
  if (b == nullptr)
    return JNI_FALSE;
  MprisState s;
  s.identity = to_utf8(env, identity);
  s.desktop_entry = to_utf8(env, package);
  s.bus_suffix = bus_element(s.desktop_entry);
  s.playback = playback == 2   ? Playback::kPlaying
               : playback == 1 ? Playback::kPaused
                               : Playback::kStopped;
  s.title = to_utf8(env, title);
  s.artist = to_utf8(env, artist);
  s.album = to_utf8(env, album);
  s.art_url = to_utf8(env, art_url);
  s.length_us = length_us > 0 ? length_us : 0;
  s.position_us = position_us > 0 ? position_us : 0;
  s.rate = rate > 0 ? static_cast<double>(rate) : 1.0;
  s.can = static_cast<unsigned>(can);
  s.track = static_cast<uint64_t>(track);
  return b->update(s) ? JNI_TRUE : JNI_FALSE;
}

// MprisBridge.nClear()
void JNICALL n_clear(JNIEnv*, jclass) {
  if (IMprisBackend* b = backend().get())
    b->clear();
}

// A command from the desktop to MprisBridge.onCommand(int, long). Runs on the
// D-Bus event-loop thread, so it is attached to the VM first.
void dispatch_command(const MprisCommand& c) {
  if (g_vm == nullptr || g_bridge == nullptr || g_on_command == nullptr)
    return;
  JNIEnv* env = nullptr;
  if (g_vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) !=
      JNI_OK)
    return;
  env->CallStaticVoidMethod(g_bridge, g_on_command, static_cast<jint>(c.kind),
                            static_cast<jlong>(c.arg));
  if (env->ExceptionCheck())
    env->ExceptionClear();
}

}  // namespace

void register_mpris_natives(JNIEnv* env) {
  jclass cls = env->FindClass("org/acl/internal/MprisBridge");
  if (cls == nullptr) {
    env->ExceptionClear();
    return;  // no host layer on this classpath
  }
  if (!backend())
    backend() = make_mpris_backend();
  const JNINativeMethod m[] = {
      {const_cast<char*>("nUpdate"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;I"
                         "Ljava/lang/String;Ljava/lang/String;"
                         "Ljava/lang/String;Ljava/lang/String;JJFIJ)Z"),
       reinterpret_cast<void*>(&n_update)},
      {const_cast<char*>("nClear"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&n_clear)},
  };
  if (env->RegisterNatives(cls, m, 2) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(MprisBridge) failed");
    env->DeleteLocalRef(cls);
    return;
  }
  env->GetJavaVM(&g_vm);
  g_bridge = static_cast<jclass>(env->NewGlobalRef(cls));
  g_on_command = env->GetStaticMethodID(g_bridge, "onCommand", "(IJ)V");
  if (g_on_command == nullptr)
    env->ExceptionClear();
  else
    backend()->set_sink(&dispatch_command);
  log::i(kTag, "media session natives registered (backend: {})",
         backend()->name());
  env->DeleteLocalRef(cls);
}

}  // namespace acl::mpris

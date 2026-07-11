// SPDX-License-Identifier: Apache-2.0
// Provenance: docs/framework-bootstrap.md §A3; JNI Functions docs (RegisterNatives,
// GetStringUTFChars). android.util.Log priority model from Android SDK docs (also
// mirrored by acl::log::Level). No GPL-3.0 sources.
//
// The JNI "native underside": acl C++ implementations of android.* native
// methods, registered against the vendored framework classes on the boot
// classpath. This is the seam by which the Java framework reaches the acl
// native core (svc/, gfx/, wm/, log/). G2.b wires the first one: android.util.Log
// -> acl::log.

#include "runtime/framework_natives.h"

#include <array>
#include <cstdint>
#include <string>

#include "audio/audio.h"
#include "gfx/gfx.h"
#include "log/log.h"
#include "svc/svc.h"

namespace acl::runtime {
namespace {

std::string to_utf8(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out(chars != nullptr ? chars : "");
  if (chars != nullptr) env->ReleaseStringUTFChars(s, chars);
  return out;
}

// android.util.Log.write0(priority, tag, msg) -> acl::log::write. Priorities
// are identical between android.util.Log and acl::log::Level (2..7).
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

// --- android.media.AudioTrack: 16-bit PCM -> acl::audio::Sink ---------------
// The sink pointer is carried across JNI as the AudioTrack.nativeHandle jlong.

jlong JNICALL AudioTrack_nativeOpen(JNIEnv* /*env*/, jclass /*cls*/, jint sample_rate,
                                    jint channels) {
  auto sink = audio::make_sink();
  audio::Format fmt{sample_rate, channels};
  if (!sink->open(fmt)) {
    log::write(log::Level::Warn, "acl.audio", "AudioTrack open failed");
  }
  return reinterpret_cast<jlong>(sink.release());  // handle owns the sink
}

jint JNICALL AudioTrack_nativeWrite(JNIEnv* env, jclass /*cls*/, jlong handle,
                                    jshortArray data, jint offset, jint size) {
  auto* sink = reinterpret_cast<audio::Sink*>(handle);
  if (sink == nullptr || data == nullptr || size <= 0) return 0;
  const jsize len = env->GetArrayLength(data);
  if (offset < 0 || size > len - offset) return -1;  // AudioTrack.ERROR_BAD_VALUE-ish
  jshort* elems = env->GetShortArrayElements(data, nullptr);
  if (elems == nullptr) return 0;
  const int written = sink->write(reinterpret_cast<std::int16_t*>(elems) + offset, size);
  env->ReleaseShortArrayElements(data, elems, JNI_ABORT);  // read-only, no copy-back
  return written;
}

void JNICALL AudioTrack_nativeStart(JNIEnv* /*env*/, jclass /*cls*/, jlong /*handle*/) {
  // The sink streams as data arrives; play() is a no-op for this backend.
}

void JNICALL AudioTrack_nativeStop(JNIEnv* /*env*/, jclass /*cls*/, jlong handle) {
  if (auto* sink = reinterpret_cast<audio::Sink*>(handle)) sink->drain();
}

void JNICALL AudioTrack_nativeRelease(JNIEnv* /*env*/, jclass /*cls*/, jlong handle) {
  if (auto* sink = reinterpret_cast<audio::Sink*>(handle)) {
    sink->close();
    delete sink;
  }
}

// --- android.media.AudioManager -> acl::svc::AudioService (in-process hub) ---
jint JNICALL AudioManager_getStreamVolume(JNIEnv* /*e*/, jclass /*c*/, jint stream) {
  return svc::audio().stream_volume(stream);
}
void JNICALL AudioManager_setStreamVolume(JNIEnv* /*e*/, jclass /*c*/, jint stream,
                                          jint index) {
  svc::audio().set_stream_volume(stream, index);
}
jint JNICALL AudioManager_getStreamMaxVolume(JNIEnv* /*e*/, jclass /*c*/, jint stream) {
  return svc::audio().stream_max_volume(stream);
}
jboolean JNICALL AudioManager_isMusicActive(JNIEnv* /*e*/, jclass /*c*/) {
  return svc::audio().music_active() ? JNI_TRUE : JNI_FALSE;
}

// --- android.content.SharedPreferences(.Editor) -> acl::svc::PreferencesService
void JNICALL Prefs_nativePutString(JNIEnv* env, jclass /*c*/, jstring name, jstring key,
                                   jstring value) {
  svc::prefs().put_string(to_utf8(env, name), to_utf8(env, key), to_utf8(env, value));
}
void JNICALL Prefs_nativePutInt(JNIEnv* env, jclass /*c*/, jstring name, jstring key,
                                jint value) {
  svc::prefs().put_int(to_utf8(env, name), to_utf8(env, key), value);
}
jstring JNICALL Prefs_nativeGetString(JNIEnv* env, jclass /*c*/, jstring name, jstring key) {
  const std::string* v = svc::prefs().find_string(to_utf8(env, name), to_utf8(env, key));
  return v != nullptr ? env->NewStringUTF(v->c_str()) : nullptr;  // null -> shim uses default
}
jint JNICALL Prefs_nativeGetInt(JNIEnv* env, jclass /*c*/, jstring name, jstring key,
                                jint def_value) {
  return svc::prefs().get_int(to_utf8(env, name), to_utf8(env, key), def_value);
}

// --- ActivityManager / WindowManager.Display / MediaSessionManager -> svc/ ------
jint JNICALL ActivityManager_getMemoryClass(JNIEnv*, jclass) {
  return svc::activity().memory_class();
}
jboolean JNICALL ActivityManager_isLowRamDevice(JNIEnv*, jclass) {
  return svc::activity().low_ram_device() ? JNI_TRUE : JNI_FALSE;
}
jint JNICALL Display_getWidth(JNIEnv*, jclass) { return svc::window().display_width(); }
jint JNICALL Display_getHeight(JNIEnv*, jclass) { return svc::window().display_height(); }
jint JNICALL Display_getRotation(JNIEnv*, jclass) { return svc::window().rotation(); }
jint JNICALL MediaSessionManager_activeCount(JNIEnv*, jclass) {
  return svc::media_sessions().active_sessions();
}
void JNICALL MediaSession_register(JNIEnv*, jclass) { svc::media_sessions().register_session(); }

// --- android.graphics.Bitmap / Canvas -> acl::gfx::Surface ------------------
jlong JNICALL Bitmap_nativeCreate(JNIEnv*, jclass, jint w, jint h) {
  return reinterpret_cast<jlong>(new gfx::Surface(w, h));
}
jint JNICALL Bitmap_nativeGetPixel(JNIEnv*, jclass, jlong ptr, jint x, jint y) {
  auto* s = reinterpret_cast<gfx::Surface*>(ptr);
  return s != nullptr ? static_cast<jint>(s->pixel(x, y)) : 0;
}
void JNICALL Bitmap_nativeDescribe(JNIEnv*, jclass, jlong ptr) {
  auto* s = reinterpret_cast<gfx::Surface*>(ptr);
  if (s == nullptr) return;
  log::i("acl.gfx", "bitmap {}x{} pixel(0,0)={:#010x} pixel({},{})={:#010x}",
         s->width(), s->height(), s->pixel(0, 0), s->width() / 2, s->height() / 2,
         s->pixel(s->width() / 2, s->height() / 2));
}
void JNICALL Canvas_nativeDrawColor(JNIEnv*, jclass, jlong ptr, jint color) {
  if (auto* s = reinterpret_cast<gfx::Surface*>(ptr))
    s->draw_color(static_cast<std::uint32_t>(color));
}
void JNICALL Canvas_nativeDrawRect(JNIEnv*, jclass, jlong ptr, jfloat l, jfloat t,
                                   jfloat r, jfloat b, jint color) {
  if (auto* s = reinterpret_cast<gfx::Surface*>(ptr)) {
    s->fill_rect(static_cast<int>(l), static_cast<int>(t), static_cast<int>(r),
                 static_cast<int>(b), static_cast<std::uint32_t>(color));
  }
}

// Bind `n` static natives on class `name`; returns false (cleared) if the class or
// registration is absent.
bool bind(JNIEnv* env, const char* name, const JNINativeMethod* methods, int n) {
  jclass cls = env->FindClass(name);
  if (cls == nullptr) { env->ExceptionClear(); return false; }
  const bool ok = env->RegisterNatives(cls, methods, n) == JNI_OK;
  if (!ok) env->ExceptionClear();
  env->DeleteLocalRef(cls);
  return ok;
}

}  // namespace

void register_framework_natives(JNIEnv* env) {
  if (env == nullptr) return;

  // android.util.Log — the first vendored framework class with an acl native.
  if (jclass log_cls = env->FindClass("android/util/Log"); log_cls != nullptr) {
    const std::array<JNINativeMethod, 1> methods{{
        {const_cast<char*>("write0"),
         const_cast<char*>("(ILjava/lang/String;Ljava/lang/String;)I"),
         reinterpret_cast<void*>(&Log_write0)},
    }};
    if (env->RegisterNatives(log_cls, methods.data(),
                             static_cast<jint>(methods.size())) != JNI_OK) {
      env->ExceptionClear();
      log::write(log::Level::Warn, "acl.runtime.jvm",
                 "RegisterNatives(android.util.Log) failed");
    } else {
      log::write(log::Level::Info, "acl.runtime.jvm",
                 "framework native bound: android.util.Log");
    }
    env->DeleteLocalRef(log_cls);
  } else {
    env->ExceptionClear();  // no framework on the boot classpath -> nothing to bind
  }

  // android.media.AudioTrack -> acl::audio (PipeWire / counting sink).
  if (jclass at_cls = env->FindClass("android/media/AudioTrack"); at_cls != nullptr) {
    const std::array<JNINativeMethod, 5> methods{{
        {const_cast<char*>("nativeOpen"), const_cast<char*>("(II)J"),
         reinterpret_cast<void*>(&AudioTrack_nativeOpen)},
        {const_cast<char*>("nativeWrite"), const_cast<char*>("(J[SII)I"),
         reinterpret_cast<void*>(&AudioTrack_nativeWrite)},
        {const_cast<char*>("nativeStart"), const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(&AudioTrack_nativeStart)},
        {const_cast<char*>("nativeStop"), const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(&AudioTrack_nativeStop)},
        {const_cast<char*>("nativeRelease"), const_cast<char*>("(J)V"),
         reinterpret_cast<void*>(&AudioTrack_nativeRelease)},
    }};
    if (env->RegisterNatives(at_cls, methods.data(),
                             static_cast<jint>(methods.size())) != JNI_OK) {
      env->ExceptionClear();
      log::write(log::Level::Warn, "acl.runtime.jvm",
                 "RegisterNatives(android.media.AudioTrack) failed");
    } else {
      log::write(log::Level::Info, "acl.runtime.jvm",
                 "framework native bound: android.media.AudioTrack");
    }
    env->DeleteLocalRef(at_cls);
  } else {
    env->ExceptionClear();
  }

  // android.media.AudioManager -> acl::svc::AudioService (the svc/ hub boundary).
  if (jclass am_cls = env->FindClass("android/media/AudioManager"); am_cls != nullptr) {
    const std::array<JNINativeMethod, 4> methods{{
        {const_cast<char*>("nativeGetStreamVolume"), const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(&AudioManager_getStreamVolume)},
        {const_cast<char*>("nativeSetStreamVolume"), const_cast<char*>("(II)V"),
         reinterpret_cast<void*>(&AudioManager_setStreamVolume)},
        {const_cast<char*>("nativeGetStreamMaxVolume"), const_cast<char*>("(I)I"),
         reinterpret_cast<void*>(&AudioManager_getStreamMaxVolume)},
        {const_cast<char*>("nativeIsMusicActive"), const_cast<char*>("()Z"),
         reinterpret_cast<void*>(&AudioManager_isMusicActive)},
    }};
    if (env->RegisterNatives(am_cls, methods.data(),
                             static_cast<jint>(methods.size())) != JNI_OK) {
      env->ExceptionClear();
      log::write(log::Level::Warn, "acl.runtime.jvm",
                 "RegisterNatives(android.media.AudioManager) failed");
    } else {
      log::write(log::Level::Info, "acl.runtime.jvm",
                 "framework native bound: android.media.AudioManager");
    }
    env->DeleteLocalRef(am_cls);
  } else {
    env->ExceptionClear();
  }

  // android.content.SharedPreferences (getters) + .Editor (putters) -> svc/ prefs.
  if (jclass sp_cls = env->FindClass("android/content/SharedPreferences"); sp_cls != nullptr) {
    const std::array<JNINativeMethod, 2> methods{{
        {const_cast<char*>("nativeGetString"),
         const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
         reinterpret_cast<void*>(&Prefs_nativeGetString)},
        {const_cast<char*>("nativeGetInt"),
         const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;I)I"),
         reinterpret_cast<void*>(&Prefs_nativeGetInt)},
    }};
    if (env->RegisterNatives(sp_cls, methods.data(), 2) != JNI_OK) env->ExceptionClear();
    env->DeleteLocalRef(sp_cls);
  } else {
    env->ExceptionClear();
  }
  if (jclass ed_cls = env->FindClass("android/content/SharedPreferences$Editor");
      ed_cls != nullptr) {
    const std::array<JNINativeMethod, 2> methods{{
        {const_cast<char*>("nativePutString"),
         const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"),
         reinterpret_cast<void*>(&Prefs_nativePutString)},
        {const_cast<char*>("nativePutInt"),
         const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;I)V"),
         reinterpret_cast<void*>(&Prefs_nativePutInt)},
    }};
    if (env->RegisterNatives(ed_cls, methods.data(), 2) != JNI_OK) env->ExceptionClear();
    env->DeleteLocalRef(ed_cls);
    log::write(log::Level::Info, "acl.runtime.jvm",
               "framework native bound: android.content.SharedPreferences");
  } else {
    env->ExceptionClear();
  }

  // More svc/ Managers (ActivityManager, WindowManager.Display, MediaSessionManager).
  const JNINativeMethod am_m[] = {
      {const_cast<char*>("nativeGetMemoryClass"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&ActivityManager_getMemoryClass)},
      {const_cast<char*>("nativeIsLowRamDevice"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&ActivityManager_isLowRamDevice)},
  };
  bind(env, "android/app/ActivityManager", am_m, 2);
  const JNINativeMethod disp_m[] = {
      {const_cast<char*>("nativeGetWidth"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&Display_getWidth)},
      {const_cast<char*>("nativeGetHeight"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&Display_getHeight)},
      {const_cast<char*>("nativeGetRotation"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&Display_getRotation)},
  };
  bind(env, "android/view/Display", disp_m, 3);
  const JNINativeMethod msm_m[] = {
      {const_cast<char*>("nativeActiveSessionCount"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&MediaSessionManager_activeCount)},
  };
  bind(env, "android/media/session/MediaSessionManager", msm_m, 1);
  const JNINativeMethod ms_m[] = {
      {const_cast<char*>("nativeRegister"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MediaSession_register)},
  };
  if (bind(env, "android/media/session/MediaSession", ms_m, 1)) {
    log::write(log::Level::Info, "acl.runtime.jvm",
               "framework natives bound: ActivityManager, Display, MediaSession(Manager)");
  }

  // android.graphics.Bitmap / Canvas -> gfx/ rasterizer.
  const JNINativeMethod bmp_m[] = {
      {const_cast<char*>("nativeCreate"), const_cast<char*>("(II)J"),
       reinterpret_cast<void*>(&Bitmap_nativeCreate)},
      {const_cast<char*>("nativeGetPixel"), const_cast<char*>("(JII)I"),
       reinterpret_cast<void*>(&Bitmap_nativeGetPixel)},
      {const_cast<char*>("nativeDescribe"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&Bitmap_nativeDescribe)},
  };
  bind(env, "android/graphics/Bitmap", bmp_m, 3);
  const JNINativeMethod canvas_m[] = {
      {const_cast<char*>("nativeDrawColor"), const_cast<char*>("(JI)V"),
       reinterpret_cast<void*>(&Canvas_nativeDrawColor)},
      {const_cast<char*>("nativeDrawRect"), const_cast<char*>("(JFFFFI)V"),
       reinterpret_cast<void*>(&Canvas_nativeDrawRect)},
  };
  if (bind(env, "android/graphics/Canvas", canvas_m, 2)) {
    log::write(log::Level::Info, "acl.runtime.jvm",
               "framework natives bound: android.graphics.Canvas/Bitmap -> gfx");
  }
}

}  // namespace acl::runtime

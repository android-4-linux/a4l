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
#include <cstdint>
#include <string>

#include "audio/audio.h"
#include "log/log.h"
#include "svc/svc.h"

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

// --- android.media.AudioTrack: 16-bit PCM -> aclcxx::audio::Sink ---------------
// The sink pointer is carried across JNI as the AudioTrack.nativeHandle jlong.

jlong JNICALL AudioTrack_nativeOpen(JNIEnv* /*env*/, jclass /*cls*/, jint sample_rate,
                                    jint channels) {
  auto sink = audio::make_sink();
  audio::Format fmt{sample_rate, channels};
  if (!sink->open(fmt)) {
    log::write(log::Level::Warn, "aclcxx.audio", "AudioTrack open failed");
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

// --- android.media.AudioManager -> aclcxx::svc::AudioService (in-process hub) ---
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

  // android.media.AudioTrack -> aclcxx::audio (PipeWire / counting sink).
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
      log::write(log::Level::Warn, "aclcxx.runtime.jvm",
                 "RegisterNatives(android.media.AudioTrack) failed");
    } else {
      log::write(log::Level::Info, "aclcxx.runtime.jvm",
                 "framework native bound: android.media.AudioTrack");
    }
    env->DeleteLocalRef(at_cls);
  } else {
    env->ExceptionClear();
  }

  // android.media.AudioManager -> aclcxx::svc::AudioService (the svc/ hub boundary).
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
      log::write(log::Level::Warn, "aclcxx.runtime.jvm",
                 "RegisterNatives(android.media.AudioManager) failed");
    } else {
      log::write(log::Level::Info, "aclcxx.runtime.jvm",
                 "framework native bound: android.media.AudioManager");
    }
    env->DeleteLocalRef(am_cls);
  } else {
    env->ExceptionClear();
  }
}

}  // namespace aclcxx::runtime

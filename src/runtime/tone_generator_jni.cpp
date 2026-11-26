// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.ToneGenerator native contract from AOSP
// frameworks/base/media/java/android/media/ToneGenerator.java (the
// Java-declared native method names and signatures, and the mNativeContext
// field) and the SDK documentation for ToneGenerator. No GPL-3.0 sources.
#include "runtime/tone_generator_jni.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>

#include "audio/tone_generator.h"
#include "log/log.h"

namespace acl::runtime {
namespace {

using audio::TonePlayer;

constexpr std::string_view kTag = "acl.audio.tone";

struct Generator {
  explicit Generator(int volume)
      : player(std::make_shared<TonePlayer>(volume)) {}
  std::shared_ptr<TonePlayer> player;
  jint session = 0;
};

// Guards mNativeContext: release() may run while another thread starts a
// tone on the same object.
std::mutex g_mu;
jfieldID g_context = nullptr;

Generator* generator_locked(JNIEnv* env, jobject thiz) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<Generator*>(env->GetLongField(thiz, g_context));
}

std::shared_ptr<TonePlayer> player_of(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(g_mu);
  const Generator* g = generator_locked(env, thiz);
  return g != nullptr ? g->player : nullptr;
}

void JNICALL TG_native_setup(JNIEnv* env,
                             jobject thiz,
                             jint /*stream_type*/,
                             jint volume,
                             jstring /*op_package*/) {
  static std::atomic<jint> next_session{30000};
  auto g = std::make_unique<Generator>(volume);
  g->session = next_session.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mu);
  delete generator_locked(env, thiz);
  env->SetLongField(thiz, g_context, reinterpret_cast<jlong>(g.release()));
}

void JNICALL TG_release(JNIEnv* env, jobject thiz) {
  std::unique_ptr<Generator> g;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g.reset(generator_locked(env, thiz));
    env->SetLongField(thiz, g_context, 0);
  }
  // The player, and its thread, end here unless a call still holds it.
}

jboolean JNICALL TG_startTone(JNIEnv* env,
                              jobject thiz,
                              jint tone,
                              jint duration_ms) {
  const std::shared_ptr<TonePlayer> player = player_of(env, thiz);
  if (!player)
    return JNI_FALSE;
  return player->start(tone, duration_ms) ? JNI_TRUE : JNI_FALSE;
}

void JNICALL TG_stopTone(JNIEnv* env, jobject thiz) {
  if (const std::shared_ptr<TonePlayer> player = player_of(env, thiz))
    player->stop();
}

jint JNICALL TG_getAudioSessionId(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(g_mu);
  const Generator* g = generator_locked(env, thiz);
  return g != nullptr ? g->session : 0;
}

}  // namespace

void register_tone_generator_natives(JNIEnv* env) {
  jclass cls = env->FindClass("android/media/ToneGenerator");
  if (cls == nullptr) {
    env->ExceptionClear();
    return;
  }
  g_context = env->GetFieldID(cls, "mNativeContext", "J");
  if (g_context == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  const JNINativeMethod methods[] = {
      {const_cast<char*>("native_setup"),
       const_cast<char*>("(IILjava/lang/String;)V"),
       reinterpret_cast<void*>(TG_native_setup)},
      {const_cast<char*>("native_finalize"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(TG_release)},
      {const_cast<char*>("release"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(TG_release)},
      {const_cast<char*>("startTone"), const_cast<char*>("(II)Z"),
       reinterpret_cast<void*>(TG_startTone)},
      {const_cast<char*>("stopTone"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(TG_stopTone)},
      {const_cast<char*>("getAudioSessionId"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(TG_getAudioSessionId)},
  };
  for (const JNINativeMethod& m : methods) {
    if (env->RegisterNatives(cls, &m, 1) != JNI_OK) {
      env->ExceptionClear();
      log::w(kTag, "ToneGenerator: could not register {}", m.name);
    }
  }
  env->DeleteLocalRef(cls);
}

}  // namespace acl::runtime

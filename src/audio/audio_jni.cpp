// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.AudioBridge (backs an
// IAudioService shim) to the svc/audio host PipeWire master-volume controller
// (default sink). Android's index-based stream volume maps onto the sink's
// linear channelVolumes. No GPL-3.0 sources.
#include "audio/audio_jni.h"

#include <cmath>
#include <memory>
#include <string_view>

#include "audio/pw_volume.h"
#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio.vol";

// Android index-based stream range. STREAM_MUSIC's max is device-defined; 15 is
// the classic default and gives a usable slider. All streams map to the one
// master (default sink) for now.
constexpr int kMaxIndex = 15;

std::unique_ptr<IPwVolume>& vol() {
  static std::unique_ptr<IPwVolume> v;
  return v;
}

// PipeWire channelVolumes are linear amplitude; wpctl/pavucontrol (and the
// perceptual feel of an Android slider) use the cubic mapping. Map the Android
// 0..max index perceptually:
//   linear = (index/max)^3 ,  index = round(cbrt(linear) * max)
int index_of(float linear) {
  if (linear < 0.0f)
    linear = 0.0f;
  int i = static_cast<int>(std::lround(std::cbrt(linear) * kMaxIndex));
  if (i < 0)
    i = 0;
  if (i > kMaxIndex)
    i = kMaxIndex;
  return i;
}
float linear_of(int index) {
  if (index < 0)
    index = 0;
  if (index > kMaxIndex)
    index = kMaxIndex;
  const float p = static_cast<float>(index) / kMaxIndex;
  return p * p * p;
}

jboolean JNICALL n_available(JNIEnv*, jclass) {
  const IPwVolume* v = vol().get();
  return (v != nullptr && v->available()) ? JNI_TRUE : JNI_FALSE;
}
jint JNICALL n_max_volume(JNIEnv*, jclass, jint) {
  return kMaxIndex;
}
jint JNICALL n_min_volume(JNIEnv*, jclass, jint) {
  return 0;
}
jint JNICALL n_get_volume(JNIEnv*, jclass, jint) {
  const IPwVolume* v = vol().get();
  return v != nullptr ? index_of(v->volume()) : 0;
}
void JNICALL n_set_volume(JNIEnv*, jclass, jint, jint index) {
  IPwVolume* v = vol().get();
  if (v != nullptr)
    v->set_volume(linear_of(index));
}
jboolean JNICALL n_is_muted(JNIEnv*, jclass, jint) {
  const IPwVolume* v = vol().get();
  return (v != nullptr && v->muted()) ? JNI_TRUE : JNI_FALSE;
}
void JNICALL n_set_muted(JNIEnv*, jclass, jint, jboolean mute) {
  IPwVolume* v = vol().get();
  if (v != nullptr)
    v->set_muted(mute == JNI_TRUE);
}

}  // namespace

void register_audio_natives(JNIEnv* env) {
  if (!vol())
    vol() = make_pw_volume();

  jclass cls = env->FindClass("org/acl/internal/AudioBridge");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag,
           "org.acl.internal.AudioBridge not on the boot classpath; audio "
           "natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nAvailable"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&n_available)},
      {const_cast<char*>("nMaxVolume"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&n_max_volume)},
      {const_cast<char*>("nMinVolume"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&n_min_volume)},
      {const_cast<char*>("nGetVolume"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&n_get_volume)},
      {const_cast<char*>("nSetVolume"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(&n_set_volume)},
      {const_cast<char*>("nIsMuted"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&n_is_muted)},
      {const_cast<char*>("nSetMuted"), const_cast<char*>("(IZ)V"),
       reinterpret_cast<void*>(&n_set_muted)},
  };
  if (env->RegisterNatives(cls, m, sizeof(m) / sizeof(m[0])) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(AudioBridge) failed");
  } else {
    log::i(kTag, "audio natives registered (master volume {})",
           (vol() && vol()->available()) ? "available" : "unavailable");
  }
  env->DeleteLocalRef(cls);
}

}  // namespace acl::audio

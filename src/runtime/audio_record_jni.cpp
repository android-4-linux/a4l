// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.AudioRecord native contract from AOSP
// frameworks/base/media/java/android/media/AudioRecord.java (the
// Java-declared native method names and signatures, and the
// mNativeAudioRecordHandle field) and the SDK documentation for AudioRecord.
// No GPL-3.0 sources.
#include "runtime/audio_record_jni.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string_view>

#include "audio/audio_record.h"
#include "log/log.h"

namespace acl::runtime {
namespace {

using audio::Recorder;

constexpr std::string_view kTag = "acl.audio.record";

// AudioFormat.ENCODING_*
constexpr jint kPcm16 = 2;
constexpr jint kPcm8 = 3;
constexpr jint kPcmFloat = 4;

// AudioSystem status codes, as AudioRecord's Java side reads them.
constexpr jint kError = -1;
constexpr jint kBadValue = -2;
constexpr jint kInvalidOperation = -3;
// AudioRecord.AUDIORECORD_ERROR_SETUP_*
constexpr jint kSetupZeroFrames = -16;
constexpr jint kSetupBadChannels = -17;
constexpr jint kSetupBadFormat = -18;
constexpr jint kSetupFailed = -20;

// Frames converted per pass: a read of any size uses this much stack.
constexpr int kChunkSamples = 2048;

int bytes_per_sample(jint format) {
  switch (format) {
    case kPcm16:
      return 2;
    case kPcm8:
      return 1;
    case kPcmFloat:
      return 4;
    default:
      return 0;
  }
}

struct Record {
  std::shared_ptr<Recorder> recorder;
  jint format = kPcm16;
  jint session = 0;
  std::atomic<jint> marker{0};
  std::atomic<jint> period{0};
};

// Guards mNativeAudioRecordHandle: release() may run while another thread
// reads from the same object.
std::mutex g_mu;
jfieldID g_handle = nullptr;
jfieldID g_ts_frames = nullptr;
jfieldID g_ts_nanos = nullptr;

Record* record_locked(JNIEnv* env, jobject thiz) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<Record*>(env->GetLongField(thiz, g_handle));
}

// The recorder and its sample format, held past the lock so a read can block.
struct Held {
  std::shared_ptr<Recorder> recorder;
  jint format = kPcm16;
};

Held held(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(g_mu);
  const Record* r = record_locked(env, thiz);
  return r != nullptr ? Held{r->recorder, r->format} : Held{};
}

jint JNICALL AR_native_setup(JNIEnv* env,
                             jobject thiz,
                             jobject /*weak_this*/,
                             jobject /*attributes*/,
                             jintArray sample_rate,
                             jint channel_mask,
                             jint channel_index_mask,
                             jint format,
                             jint buffer_bytes,
                             jintArray session_id,
                             jobject /*attribution_source*/,
                             jlong /*native_record*/,
                             jint /*shared_history_ms*/) {
  static std::atomic<jint> next_session{40000};
  if (sample_rate == nullptr || env->GetArrayLength(sample_rate) < 1 ||
      session_id == nullptr || env->GetArrayLength(session_id) < 1)
    return kSetupFailed;
  const int bps = bytes_per_sample(format);
  if (bps == 0)
    return kSetupBadFormat;
  const int channels = std::popcount(static_cast<std::uint32_t>(
      channel_index_mask != 0 ? channel_index_mask : channel_mask));
  if (channels < 1)
    return kSetupBadChannels;

  jint rate = 0;
  env->GetIntArrayRegion(sample_rate, 0, 1, &rate);
  if (rate == 0)
    rate = 48000;  // unspecified: the source's own rate

  Recorder::Config cfg;
  cfg.sample_rate = rate;
  cfg.channels = channels;
  const int frames = buffer_bytes / (channels * bps);
  if (frames < 1)
    return kSetupZeroFrames;
  cfg.capacity_frames = std::max(frames, Recorder::min_capacity_frames(rate));
  if (!Recorder::valid(cfg)) {
    log::w(kTag, "AudioRecord refused: {} Hz, {} ch, {} frames", rate, channels,
           frames);
    return kSetupFailed;
  }

  auto r = std::make_unique<Record>();
  r->recorder = std::make_shared<Recorder>(cfg, audio::make_source());
  if (!r->recorder->ok())
    return kSetupFailed;
  r->format = format;
  jint session = 0;
  env->GetIntArrayRegion(session_id, 0, 1, &session);
  if (session == 0)
    session = next_session.fetch_add(1, std::memory_order_relaxed);
  r->session = session;
  env->SetIntArrayRegion(session_id, 0, 1, &session);
  env->SetIntArrayRegion(sample_rate, 0, 1, &rate);
  log::i(kTag, "AudioRecord: {} Hz, {} ch, {} frames (silence: no microphone)",
         rate, channels, cfg.capacity_frames);

  std::unique_ptr<Record> old;
  std::lock_guard<std::mutex> lock(g_mu);
  old.reset(record_locked(env, thiz));
  if (old)
    old->recorder->shutdown();
  env->SetLongField(thiz, g_handle, reinterpret_cast<jlong>(r.release()));
  return 0;
}

void JNICALL AR_native_release(JNIEnv* env, jobject thiz) {
  std::unique_ptr<Record> r;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    r.reset(record_locked(env, thiz));
    env->SetLongField(thiz, g_handle, 0);
  }
  // A read still blocked on it returns; the recorder ends when that read
  // lets go of it.
  if (r)
    r->recorder->shutdown();
}

jint JNICALL AR_native_start(JNIEnv* env,
                             jobject thiz,
                             jint /*sync_event*/,
                             jint /*session*/) {
  const Held h = held(env, thiz);
  if (!h.recorder)
    return kInvalidOperation;
  h.recorder->start();
  return 0;
}

void JNICALL AR_native_stop(JNIEnv* env, jobject thiz) {
  if (const Held h = held(env, thiz); h.recorder)
    h.recorder->stop();
}

// Read `count` samples and hand each converted chunk to `put(pcm, done, n)`,
// which returns false if it could not store them. Returns samples read, or a
// status code when nothing was.
template <typename Put>
jint read_samples(const Held& h, jint count, bool blocking, Put put) {
  if (!h.recorder)
    return kInvalidOperation;
  const int channels = h.recorder->config().channels;
  std::array<std::int16_t, kChunkSamples> pcm{};
  const int chunk_frames = kChunkSamples / channels;
  jint done = 0;
  const jint frames = count / channels;
  while (done < frames * channels) {
    const int want = std::min(chunk_frames, frames - done / channels);
    const int got = h.recorder->read(pcm.data(), want, blocking);
    if (got < 0)
      return done > 0 ? done : kInvalidOperation;
    if (got == 0)
      break;
    if (!put(pcm.data(), done, got * channels))
      return done > 0 ? done : kError;
    done += got * channels;
    if (got < want)
      break;
  }
  return done;
}

jint JNICALL AR_read_bytes(JNIEnv* env,
                           jobject thiz,
                           jbyteArray data,
                           jint offset,
                           jint size,
                           jboolean blocking) {
  const Held h = held(env, thiz);
  if (data == nullptr || offset < 0 || size < 0 ||
      size > env->GetArrayLength(data) - offset)
    return kBadValue;
  if (h.format == kPcmFloat)
    return kInvalidOperation;  // floats are read as floats or into a buffer
  const int bps = bytes_per_sample(h.format);
  const jint n = read_samples(
      h, size / bps, blocking == JNI_TRUE,
      [&](const std::int16_t* pcm, jint at, jint count) {
        std::array<jbyte, static_cast<std::size_t>(kChunkSamples) * 2> out{};
        if (bps == 2) {
          std::memcpy(out.data(), pcm, static_cast<std::size_t>(count) * 2);
        } else {
          for (jint i = 0; i < count; ++i)
            out[static_cast<std::size_t>(i)] =
                static_cast<jbyte>((pcm[i] >> 8) + 128);
        }
        env->SetByteArrayRegion(data, offset + at * bps, count * bps,
                                out.data());
        return env->ExceptionCheck() == JNI_FALSE;
      });
  return n > 0 ? n * bps : n;
}

jint JNICALL AR_read_shorts(JNIEnv* env,
                            jobject thiz,
                            jshortArray data,
                            jint offset,
                            jint size,
                            jboolean blocking) {
  const Held h = held(env, thiz);
  if (data == nullptr || offset < 0 || size < 0 ||
      size > env->GetArrayLength(data) - offset)
    return kBadValue;
  if (h.format != kPcm16)
    return kInvalidOperation;
  return read_samples(h, size, blocking == JNI_TRUE,
                      [&](const std::int16_t* pcm, jint at, jint count) {
                        env->SetShortArrayRegion(data, offset + at, count, pcm);
                        return env->ExceptionCheck() == JNI_FALSE;
                      });
}

jint JNICALL AR_read_floats(JNIEnv* env,
                            jobject thiz,
                            jfloatArray data,
                            jint offset,
                            jint size,
                            jboolean blocking) {
  const Held h = held(env, thiz);
  if (data == nullptr || offset < 0 || size < 0 ||
      size > env->GetArrayLength(data) - offset)
    return kBadValue;
  if (h.format != kPcmFloat)
    return kInvalidOperation;
  return read_samples(h, size, blocking == JNI_TRUE,
                      [&](const std::int16_t* pcm, jint at, jint count) {
                        std::array<jfloat, kChunkSamples> out{};
                        for (jint i = 0; i < count; ++i)
                          out[static_cast<std::size_t>(i)] =
                              static_cast<jfloat>(pcm[i]) / 32768.0F;
                        env->SetFloatArrayRegion(data, offset + at, count,
                                                 out.data());
                        return env->ExceptionCheck() == JNI_FALSE;
                      });
}

jint JNICALL AR_read_direct(JNIEnv* env,
                            jobject thiz,
                            jobject buffer,
                            jint size,
                            jboolean blocking) {
  const Held h = held(env, thiz);
  auto* dst =
      buffer != nullptr
          ? static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer))
          : nullptr;
  if (dst == nullptr || size < 0 || size > env->GetDirectBufferCapacity(buffer))
    return kBadValue;
  const int bps = bytes_per_sample(h.format);
  const jint n = read_samples(
      h, size / bps, blocking == JNI_TRUE,
      [&](const std::int16_t* pcm, jint at, jint count) {
        std::uint8_t* out = dst + static_cast<std::ptrdiff_t>(at) * bps;
        for (jint i = 0; i < count; ++i) {
          if (bps == 2) {
            std::memcpy(out + static_cast<std::ptrdiff_t>(i) * 2, &pcm[i], 2);
          } else if (bps == 1) {
            out[i] = static_cast<std::uint8_t>((pcm[i] >> 8) + 128);
          } else {
            const float f = static_cast<float>(pcm[i]) / 32768.0F;
            std::memcpy(out + static_cast<std::ptrdiff_t>(i) * 4, &f, 4);
          }
        }
        return true;
      });
  return n > 0 ? n * bps : n;
}

jint JNICALL AR_buffer_frames(JNIEnv* env, jobject thiz) {
  const Held h = held(env, thiz);
  return h.recorder ? h.recorder->config().capacity_frames : 0;
}

// The marker and the period are kept and read back. No event is posted for
// them: nothing here watches the position against them yet.
jint JNICALL AR_set_marker(JNIEnv* env, jobject thiz, jint marker) {
  std::lock_guard<std::mutex> lock(g_mu);
  Record* r = record_locked(env, thiz);
  if (r == nullptr)
    return kInvalidOperation;
  r->marker.store(marker, std::memory_order_relaxed);
  return 0;
}
jint JNICALL AR_get_marker(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(g_mu);
  const Record* r = record_locked(env, thiz);
  return r != nullptr ? r->marker.load(std::memory_order_relaxed) : 0;
}
jint JNICALL AR_set_period(JNIEnv* env, jobject thiz, jint period) {
  std::lock_guard<std::mutex> lock(g_mu);
  Record* r = record_locked(env, thiz);
  if (r == nullptr)
    return kInvalidOperation;
  r->period.store(period, std::memory_order_relaxed);
  return 0;
}
jint JNICALL AR_get_period(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(g_mu);
  const Record* r = record_locked(env, thiz);
  return r != nullptr ? r->period.load(std::memory_order_relaxed) : 0;
}

// Bytes for the smallest buffer: 0 (a bad value) for a rate, channel count
// or format a recorder cannot have.
jint JNICALL
AR_min_buffer_size(JNIEnv*, jclass, jint rate, jint channels, jint format) {
  const int frames = Recorder::min_capacity_frames(rate);
  const int bps = bytes_per_sample(format);
  if (frames == 0 || bps == 0 || channels < 1 || channels > 8)
    return 0;
  return frames * channels * bps;
}

jboolean JNICALL AR_set_input_device(JNIEnv*, jobject, jint) {
  return JNI_TRUE;  // there is one input
}
jint JNICALL AR_routed_device(JNIEnv*, jobject) {
  return 0;
}
void JNICALL AR_nop(JNIEnv*, jobject) {}

// The frames read so far and the time now: with no device between the source
// and the reader, the last frame read is the newest there is.
jint JNICALL AR_timestamp(JNIEnv* env,
                          jobject thiz,
                          jobject out,
                          jint timebase) {
  const Held h = held(env, thiz);
  if (!h.recorder || out == nullptr || g_ts_frames == nullptr ||
      g_ts_nanos == nullptr)
    return kInvalidOperation;
  const std::uint64_t frames = h.recorder->position();
  if (frames == 0)
    return kInvalidOperation;  // nothing recorded yet
  timespec ts{};
  // AudioTimestamp.TIMEBASE_BOOTTIME is 1, TIMEBASE_MONOTONIC is 0.
  clock_gettime(timebase == 1 ? CLOCK_BOOTTIME : CLOCK_MONOTONIC, &ts);
  env->SetLongField(out, g_ts_frames, static_cast<jlong>(frames));
  env->SetLongField(out, g_ts_nanos,
                    static_cast<jlong>(ts.tv_sec) * 1'000'000'000 +
                        static_cast<jlong>(ts.tv_nsec));
  return 0;
}

jint JNICALL AR_active_microphones(JNIEnv*, jobject, jobject) {
  return 0;  // none: the list is left empty
}
jint JNICALL AR_port_id(JNIEnv*, jobject) {
  return 0;
}
jint JNICALL AR_mic_direction(JNIEnv*, jobject, jint) {
  return kInvalidOperation;
}
jint JNICALL AR_mic_zoom(JNIEnv*, jobject, jfloat) {
  return kInvalidOperation;
}
void JNICALL AR_log_session(JNIEnv*, jobject, jstring) {}
jint JNICALL AR_share_history(JNIEnv*, jobject, jstring, jlong) {
  return kInvalidOperation;
}
jobject JNICALL AR_metrics(JNIEnv*, jobject) {
  return nullptr;
}

void JNICALL MR_native_init(JNIEnv*, jclass) {}

}  // namespace

void register_audio_record_natives(JNIEnv* env) {
  jclass cls = env->FindClass("android/media/AudioRecord");
  if (cls == nullptr) {
    env->ExceptionClear();
    return;
  }
  g_handle = env->GetFieldID(cls, "mNativeAudioRecordHandle", "J");
  if (g_handle == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  if (jclass ts = env->FindClass("android/media/AudioTimestamp")) {
    g_ts_frames = env->GetFieldID(ts, "framePosition", "J");
    g_ts_nanos = env->GetFieldID(ts, "nanoTime", "J");
    env->DeleteLocalRef(ts);
  }
  env->ExceptionClear();

  struct Method {
    const char* name;
    const char* sig;
    void* fn;
  };
  // NOLINTBEGIN(bugprone-casting-through-void)
  const Method methods[] = {
      {"native_setup",
       "(Ljava/lang/Object;Ljava/lang/Object;[IIIII[ILandroid/os/Parcel;JI)I",
       reinterpret_cast<void*>(AR_native_setup)},
      {"native_finalize", "()V", reinterpret_cast<void*>(AR_native_release)},
      {"native_release", "()V", reinterpret_cast<void*>(AR_native_release)},
      {"native_start", "(II)I", reinterpret_cast<void*>(AR_native_start)},
      {"native_stop", "()V", reinterpret_cast<void*>(AR_native_stop)},
      {"native_read_in_byte_array", "([BIIZ)I",
       reinterpret_cast<void*>(AR_read_bytes)},
      {"native_read_in_short_array", "([SIIZ)I",
       reinterpret_cast<void*>(AR_read_shorts)},
      {"native_read_in_float_array", "([FIIZ)I",
       reinterpret_cast<void*>(AR_read_floats)},
      {"native_read_in_direct_buffer", "(Ljava/lang/Object;IZ)I",
       reinterpret_cast<void*>(AR_read_direct)},
      {"native_get_buffer_size_in_frames", "()I",
       reinterpret_cast<void*>(AR_buffer_frames)},
      {"native_set_marker_pos", "(I)I", reinterpret_cast<void*>(AR_set_marker)},
      {"native_get_marker_pos", "()I", reinterpret_cast<void*>(AR_get_marker)},
      {"native_set_pos_update_period", "(I)I",
       reinterpret_cast<void*>(AR_set_period)},
      {"native_get_pos_update_period", "()I",
       reinterpret_cast<void*>(AR_get_period)},
      {"native_get_min_buff_size", "(III)I",
       reinterpret_cast<void*>(AR_min_buffer_size)},
      {"native_setInputDevice", "(I)Z",
       reinterpret_cast<void*>(AR_set_input_device)},
      {"native_getRoutedDeviceId", "()I",
       reinterpret_cast<void*>(AR_routed_device)},
      {"native_enableDeviceCallback", "()V", reinterpret_cast<void*>(AR_nop)},
      {"native_disableDeviceCallback", "()V", reinterpret_cast<void*>(AR_nop)},
      {"native_get_timestamp", "(Landroid/media/AudioTimestamp;I)I",
       reinterpret_cast<void*>(AR_timestamp)},
      {"native_get_active_microphones", "(Ljava/util/ArrayList;)I",
       reinterpret_cast<void*>(AR_active_microphones)},
      {"native_getPortId", "()I", reinterpret_cast<void*>(AR_port_id)},
      {"native_set_preferred_microphone_direction", "(I)I",
       reinterpret_cast<void*>(AR_mic_direction)},
      {"native_set_preferred_microphone_field_dimension", "(F)I",
       reinterpret_cast<void*>(AR_mic_zoom)},
      {"native_setLogSessionId", "(Ljava/lang/String;)V",
       reinterpret_cast<void*>(AR_log_session)},
      {"native_shareAudioHistory", "(Ljava/lang/String;J)I",
       reinterpret_cast<void*>(AR_share_history)},
      {"native_getMetrics", "()Landroid/os/PersistableBundle;",
       reinterpret_cast<void*>(AR_metrics)},
  };
  // NOLINTEND(bugprone-casting-through-void)
  int bound = 0;
  for (const Method& m : methods) {
    const JNINativeMethod nm{const_cast<char*>(m.name),
                             const_cast<char*>(m.sig), m.fn};
    if (env->RegisterNatives(cls, &nm, 1) == JNI_OK) {
      ++bound;
    } else {
      env->ExceptionClear();
      log::w(kTag, "AudioRecord: could not register {}", m.name);
    }
  }
  log::i(kTag, "android.media.AudioRecord natives registered ({} of {})", bound,
         std::size(methods));
  env->DeleteLocalRef(cls);

  // AudioRecord checks its audio source against MediaRecorder's list, which
  // initializes that class, and its initializer calls native_init. Bind that
  // one so the class loads. The recorder itself is not here: its other
  // natives stay unbound and say so when called.
  if (jclass mr = env->FindClass("android/media/MediaRecorder")) {
    const JNINativeMethod init{const_cast<char*>("native_init"),
                               const_cast<char*>("()V"),
                               reinterpret_cast<void*>(MR_native_init)};
    if (env->RegisterNatives(mr, &init, 1) != JNI_OK)
      log::w(kTag, "MediaRecorder: could not register native_init");
    env->DeleteLocalRef(mr);
  }
  env->ExceptionClear();
}

}  // namespace acl::runtime

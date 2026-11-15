// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.AudioTrack native contract from AOSP
// frameworks/base/media/java/android/media/AudioTrack.java (the Java-declared
// native method names and signatures, the mNativeTrackInJavaObj and
// mStreamType fields, and the error constants) and the SDK documentation for
// AudioTrack, AudioFormat and AudioTimestamp. No GPL-3.0 sources.
//
// The native side of the framework's android.media.AudioTrack on host ART.
//
// Each Java AudioTrack owns one audio::Track, which holds the buffer and
// paces playback, and through it one audio::Sink: PipeWire, the counting
// sink, or in a --split app process the one that sends to the broker. The
// Java object carries an id, not a pointer: a native call that races
// release() finds no track and returns an error instead of using freed
// memory.
//
// Samples are converted to 16-bit on the way in, since that is what a Sink
// takes. Linear PCM only: 8-bit, 16-bit, 24-bit packed, 32-bit and float.

#include "runtime/audio_track_jni.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "audio/audio.h"
#include "audio/audio_track.h"
#include "log/log.h"

namespace acl::runtime {
namespace {

constexpr std::string_view kTag = "acl.audio.track";

// android.media.AudioTrack / AudioSystem result codes.
constexpr jint kSuccess = 0;
constexpr jint kError = -1;
constexpr jint kErrorBadValue = -2;
constexpr jint kErrorInvalidOperation = -3;
constexpr jint kErrorDeadObject = -6;
// native_setup's own failures (AudioTrack.ERROR_NATIVESETUP_*).
constexpr jint kSetupAudioSystem = -16;
constexpr jint kSetupInvalidChannelMask = -17;
constexpr jint kSetupInvalidFormat = -18;
constexpr jint kSetupNativeInitFailed = -20;

// android.media.AudioFormat encodings.
constexpr jint kEncodingDefault = 1;
constexpr jint kEncodingPcm16 = 2;
constexpr jint kEncodingPcm8 = 3;
constexpr jint kEncodingPcmFloat = 4;
constexpr jint kEncodingPcm24Packed = 21;
constexpr jint kEncodingPcm32 = 22;

constexpr jint kModeStatic = 0;   // AudioTrack.MODE_STATIC
constexpr jint kStreamMusic = 3;  // AudioManager.STREAM_MUSIC
constexpr int kOutputSampleRate = 48000;
// Frames converted per step of a write: a blocking write holds no Java array
// and no large buffer while it waits.
constexpr int kChunkFrames = 2048;
// The most elements one short[] or float[] write may name: their size in
// bytes must fit an int.
constexpr jint kMaxWriteElements = 0x7fffffff / 4;

// Bytes per sample of an encoding, or 0 if it is not linear PCM a4l plays.
int bytes_per_sample(jint encoding) noexcept {
  switch (encoding) {
    case kEncodingDefault:
    case kEncodingPcm16:
      return 2;
    case kEncodingPcm8:
      return 1;
    case kEncodingPcmFloat:
    case kEncodingPcm32:
      return 4;
    case kEncodingPcm24Packed:
      return 3;
    default:
      return 0;
  }
}

std::int16_t from_float(float v) noexcept {
  if (!(v == v))
    return 0;  // NaN
  const float s = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
  return static_cast<std::int16_t>(std::lround(s));
}

// Convert `samples` samples of `encoding` at `in` to 16-bit at `out`.
// Little-endian, as every Android ABI is and as this host is.
void to_s16(jint encoding,
            const std::uint8_t* in,
            std::int16_t* out,
            std::size_t samples) noexcept {
  switch (encoding) {
    case kEncodingPcm8:  // unsigned
      for (std::size_t i = 0; i < samples; ++i)
        out[i] =
            static_cast<std::int16_t>((static_cast<int>(in[i]) - 128) * 256);
      break;
    case kEncodingPcmFloat:
      for (std::size_t i = 0; i < samples; ++i) {
        float f;
        std::memcpy(&f, in + i * 4, sizeof(f));
        out[i] = from_float(f);
      }
      break;
    case kEncodingPcm24Packed:  // the top two of three bytes
      for (std::size_t i = 0; i < samples; ++i)
        out[i] = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(in[i * 3 + 1]) |
            static_cast<std::uint16_t>(in[i * 3 + 2]) << 8);
      break;
    case kEncodingPcm32:  // the top two of four bytes
      for (std::size_t i = 0; i < samples; ++i)
        out[i] = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(in[i * 4 + 2]) |
            static_cast<std::uint16_t>(in[i * 4 + 3]) << 8);
      break;
    default:  // 16-bit
      std::memcpy(out, in, samples * sizeof(std::int16_t));
      break;
  }
}

struct TrackCtx {
  std::unique_ptr<audio::Track> track;
  jint encoding = kEncodingPcm16;
  int channels = 2;
  int sample_bytes = 2;
  // Values the app sets and reads back; a4l does not act on them.
  std::atomic<jint> marker{0};
  std::atomic<jint> period{0};
  std::atomic<jint> start_threshold{0};
};

// id -> track. An id is never reused.
struct Registry {
  std::mutex mu;
  std::unordered_map<jlong, std::shared_ptr<TrackCtx>> tracks;
  jlong next = 1;
};
Registry& registry() {
  static Registry r;
  return r;
}

jfieldID g_native_field = nullptr;  // AudioTrack.mNativeTrackInJavaObj (J)
jfieldID g_stream_type = nullptr;   // AudioTrack.mStreamType (I)

std::shared_ptr<TrackCtx> ctx_of(JNIEnv* env, jobject thiz) {
  if (g_native_field == nullptr || thiz == nullptr)
    return nullptr;
  const jlong id = env->GetLongField(thiz, g_native_field);
  if (id == 0)
    return nullptr;
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  const auto it = r.tracks.find(id);
  return it != r.tracks.end() ? it->second : nullptr;
}

void drop(JNIEnv* env, jobject thiz) {
  if (g_native_field == nullptr || thiz == nullptr)
    return;
  const jlong id = env->GetLongField(thiz, g_native_field);
  if (id == 0)
    return;
  env->SetLongField(thiz, g_native_field, 0);
  std::shared_ptr<TrackCtx> gone;
  {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    const auto it = r.tracks.find(id);
    if (it != r.tracks.end()) {
      gone = std::move(it->second);
      r.tracks.erase(it);
    }
  }
  // release() while another thread is inside write(): that thread holds the
  // track, so it cannot be destroyed yet. Ending it lets the write return,
  // and the track goes when the write does.
  if (gone && gone->track)
    gone->track->shutdown();
}

// ── setup ────────────────────────────────────────────────────────────────

jint JNICALL AT_native_setup(JNIEnv* env,
                             jobject thiz,
                             jobject /*weak_this*/,
                             jobject /*attributes*/,
                             jintArray sample_rate,
                             jint channel_mask,
                             jint channel_index_mask,
                             jint audio_format,
                             jint buffer_bytes,
                             jint mode,
                             jintArray session_id,
                             jlong native_track,
                             jboolean offload,
                             jint /*encapsulation_mode*/,
                             jobject /*tuner_configuration*/,
                             jstring /*op_package_name*/) {
  if (g_native_field == nullptr || sample_rate == nullptr ||
      session_id == nullptr || env->GetArrayLength(sample_rate) < 1 ||
      env->GetArrayLength(session_id) < 1)
    return kSetupNativeInitFailed;
  // A track made around an existing native one, or an offloaded (encoded)
  // one: neither exists here.
  if (native_track != 0 || offload == JNI_TRUE)
    return kSetupNativeInitFailed;
  const int sample_bytes = bytes_per_sample(audio_format);
  if (sample_bytes == 0) {
    log::w(kTag, "AudioTrack: encoding {} is not linear PCM; not supported",
           audio_format);
    return kSetupInvalidFormat;
  }
  // The mask is the native one: one bit per channel either way.
  const int channels = std::popcount(static_cast<std::uint32_t>(
      channel_mask != 0 ? channel_mask : channel_index_mask));
  if (channels < 1 || channels > 8)
    return kSetupInvalidChannelMask;

  jint rate = 0;
  env->GetIntArrayRegion(sample_rate, 0, 1, &rate);
  if (rate == 0)
    rate = kOutputSampleRate;  // "unspecified": the output's rate
  audio::Track::Config cfg;
  cfg.sample_rate = rate;
  cfg.channels = channels;
  cfg.static_mode = mode == kModeStatic;
  const int frame_bytes = channels * sample_bytes;
  cfg.capacity_frames = buffer_bytes > 0 ? buffer_bytes / frame_bytes : 0;
  if (!cfg.static_mode)
    cfg.capacity_frames =
        std::max(cfg.capacity_frames, audio::Track::min_capacity_frames(rate));
  if (!audio::Track::valid(cfg)) {
    log::w(kTag, "AudioTrack: {} Hz, {} ch, {} frames is out of range", rate,
           channels, cfg.capacity_frames);
    return kSetupAudioSystem;
  }

  auto ctx = std::make_shared<TrackCtx>();
  ctx->encoding = audio_format;
  ctx->channels = channels;
  ctx->sample_bytes = sample_bytes;
  ctx->start_threshold = cfg.capacity_frames / 2;
  ctx->track = std::make_unique<audio::Track>(cfg, audio::make_sink());
  if (!ctx->track->ok())
    return kSetupNativeInitFailed;

  jlong id = 0;
  {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    id = r.next++;
    r.tracks.emplace(id, std::move(ctx));
  }
  env->SetLongField(thiz, g_native_field, id);
  if (g_stream_type != nullptr)
    env->SetIntField(thiz, g_stream_type, kStreamMusic);
  env->SetIntArrayRegion(sample_rate, 0, 1, &rate);
  // Session 0 asks for one to be allocated.
  jint session = 0;
  env->GetIntArrayRegion(session_id, 0, 1, &session);
  if (session == 0) {
    static std::atomic<jint> next_session{20000};
    session = next_session.fetch_add(1, std::memory_order_relaxed);
    env->SetIntArrayRegion(session_id, 0, 1, &session);
  }
  log::i(kTag, "AudioTrack {}: {} Hz, {} ch, encoding {}, {} frames, {}", id,
         rate, channels, audio_format, cfg.capacity_frames,
         cfg.static_mode ? "static" : "streaming");
  return kSuccess;
}

void JNICALL AT_native_release(JNIEnv* env, jobject thiz) {
  drop(env, thiz);
}
void JNICALL AT_native_finalize(JNIEnv* env, jobject thiz) {
  drop(env, thiz);
}

// ── transport ────────────────────────────────────────────────────────────

// ACL_MC_DIAG: when a track is started, written and asked where it is, on
// CLOCK_MONOTONIC, to line a player's audio clock up against its video.
bool diag() {
  static const bool on = std::getenv("ACL_MC_DIAG") != nullptr;
  return on;
}
std::int64_t diag_now_ns() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

void JNICALL AT_native_start(JNIEnv* env, jobject thiz) {
  if (auto c = ctx_of(env, thiz)) {
    if (diag())
      log::i(kTag, "diag: play at {} ns, {} frames buffered", diag_now_ns(),
             c->track->buffered_frames());
    c->track->play();
  }
}
void JNICALL AT_native_stop(JNIEnv* env, jobject thiz) {
  if (auto c = ctx_of(env, thiz))
    c->track->stop();
}
void JNICALL AT_native_pause(JNIEnv* env, jobject thiz) {
  if (auto c = ctx_of(env, thiz))
    c->track->pause();
}
void JNICALL AT_native_flush(JNIEnv* env, jobject thiz) {
  if (auto c = ctx_of(env, thiz))
    c->track->flush();
}
jint JNICALL AT_native_reload_static(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  return c->track->reload_static() ? kSuccess : kErrorInvalidOperation;
}

// ── writes ───────────────────────────────────────────────────────────────

// Write `bytes` bytes of the track's encoding, fetched a piece at a time by
// `fetch(offset, count, dst)`. Returns bytes taken, or an error code.
template <typename Fetch>
jint write_bytes(TrackCtx& c, jint bytes, bool blocking, Fetch fetch) {
  const int frame_bytes = c.channels * c.sample_bytes;
  const int frames = bytes / frame_bytes;
  // Per thread, so that an app writing 10 ms at a time does not allocate a
  // hundred times a second. Sized for the widest frame: 8 channels of 4 bytes.
  thread_local std::vector<std::uint8_t> raw(kChunkFrames * 8 * 4);
  thread_local std::vector<std::int16_t> pcm(kChunkFrames * 8);
  int done = 0;
  while (done < frames) {
    const int n = std::min(kChunkFrames, frames - done);
    fetch(done * frame_bytes, n * frame_bytes, raw.data());
    to_s16(c.encoding, raw.data(), pcm.data(),
           static_cast<std::size_t>(n) * static_cast<std::size_t>(c.channels));
    const int took = c.track->write(pcm.data(), n, blocking);
    if (took < 0)
      return done > 0 ? done * frame_bytes : kErrorDeadObject;
    done += took;
    if (took < n)
      break;  // not blocking and full, or the track stopped under a blocking
              // write
  }
  return done * frame_bytes;
}

jint JNICALL AT_native_write_byte(JNIEnv* env,
                                  jobject thiz,
                                  jbyteArray data,
                                  jint offset,
                                  jint size,
                                  jint /*format*/,
                                  jboolean blocking) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  if (data == nullptr || offset < 0 || size < 0 ||
      size > env->GetArrayLength(data) - offset)
    return kErrorBadValue;
  return write_bytes(*c, size, blocking == JNI_TRUE,
                     [&](int at, int count, std::uint8_t* dst) {
                       env->GetByteArrayRegion(data, offset + at, count,
                                               reinterpret_cast<jbyte*>(dst));
                     });
}

jint JNICALL AT_native_write_short(JNIEnv* env,
                                   jobject thiz,
                                   jshortArray data,
                                   jint offset,
                                   jint size,
                                   jint /*format*/,
                                   jboolean blocking) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  if (data == nullptr || offset < 0 || size < 0 ||
      size > env->GetArrayLength(data) - offset)
    return kErrorBadValue;
  // A short[] write to a track that is not 16-bit.
  if (c->sample_bytes != 2)
    return kErrorInvalidOperation;
  if (size > kMaxWriteElements)
    return kErrorBadValue;
  const jint bytes =
      write_bytes(*c, size * 2, blocking == JNI_TRUE,
                  [&](int at, int count, std::uint8_t* dst) {
                    env->GetShortArrayRegion(data, offset + at / 2, count / 2,
                                             reinterpret_cast<jshort*>(dst));
                  });
  return bytes < 0 ? bytes : bytes / 2;
}

jint JNICALL AT_native_write_float(JNIEnv* env,
                                   jobject thiz,
                                   jfloatArray data,
                                   jint offset,
                                   jint size,
                                   jint /*format*/,
                                   jboolean blocking) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  if (data == nullptr || offset < 0 || size < 0 ||
      size > env->GetArrayLength(data) - offset)
    return kErrorBadValue;
  if (c->encoding != kEncodingPcmFloat)
    return kErrorInvalidOperation;
  if (size > kMaxWriteElements)
    return kErrorBadValue;
  const jint bytes =
      write_bytes(*c, size * 4, blocking == JNI_TRUE,
                  [&](int at, int count, std::uint8_t* dst) {
                    env->GetFloatArrayRegion(data, offset + at / 4, count / 4,
                                             reinterpret_cast<jfloat*>(dst));
                  });
  return bytes < 0 ? bytes : bytes / 4;
}

jint JNICALL AT_native_write_native_bytes(JNIEnv* env,
                                          jobject thiz,
                                          jobject buffer,
                                          jint position,
                                          jint size,
                                          jint /*format*/,
                                          jboolean blocking) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  if (buffer == nullptr || position < 0 || size < 0)
    return kErrorBadValue;
  const auto* base =
      static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(buffer));
  const jlong capacity = env->GetDirectBufferCapacity(buffer);
  if (base == nullptr || capacity < 0 ||
      static_cast<jlong>(position) + size > capacity)
    return kErrorBadValue;
  return write_bytes(*c, size, blocking == JNI_TRUE,
                     [&](int at, int count, std::uint8_t* dst) {
                       std::memcpy(dst, base + position + at,
                                   static_cast<std::size_t>(count));
                     });
}

// ── position and state ───────────────────────────────────────────────────

jint JNICALL AT_native_get_position(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  if (c && diag()) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) % 50 == 0)
      log::i(kTag, "diag: position {} at {} ns, {} buffered, {} to the sink",
             c->track->position(), diag_now_ns(), c->track->buffered_frames(),
             c->track->frames_to_sink());
  }
  // The head position is a 32-bit count that wraps.
  return c ? static_cast<jint>(static_cast<std::uint32_t>(c->track->position()))
           : 0;
}
jint JNICALL AT_native_set_position(JNIEnv*, jobject, jint) {
  return kErrorInvalidOperation;  // static-track seeking: not built
}
jint JNICALL AT_native_get_timestamp(JNIEnv* env,
                                     jobject thiz,
                                     jlongArray out) {
  auto c = ctx_of(env, thiz);
  if (!c || out == nullptr || env->GetArrayLength(out) < 2)
    return kErrorInvalidOperation;
  std::uint64_t frames = 0;
  std::int64_t ns = 0;
  if (!c->track->timestamp(&frames, &ns))
    return kErrorInvalidOperation;  // nothing has played yet
  const jlong v[2] = {static_cast<jlong>(frames), static_cast<jlong>(ns)};
  env->SetLongArrayRegion(out, 0, 2, v);
  return kSuccess;
}
jint JNICALL AT_native_get_latency(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return 0;
  const audio::Track::Config& cfg = c->track->config();
  return static_cast<jint>(static_cast<std::int64_t>(cfg.capacity_frames) *
                           1000 / cfg.sample_rate);
}
jint JNICALL AT_native_get_underrun_count(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  return c ? c->track->underruns() : 0;
}
jint JNICALL AT_native_get_flags(JNIEnv*, jobject) {
  return 0;
}
jint JNICALL AT_native_get_buffer_frames(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  return c ? c->track->config().capacity_frames : kErrorInvalidOperation;
}
jint JNICALL AT_native_set_buffer_frames(JNIEnv* env, jobject thiz, jint) {
  // The buffer keeps the size it was made with; the app is told what it has.
  auto c = ctx_of(env, thiz);
  return c ? c->track->config().capacity_frames : kErrorInvalidOperation;
}
void JNICALL AT_native_setVolume(JNIEnv* env,
                                 jobject thiz,
                                 jfloat left,
                                 jfloat right) {
  if (auto c = ctx_of(env, thiz))
    c->track->set_volume(left, right);
}
jint JNICALL AT_native_get_playback_rate(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  return c ? c->track->config().sample_rate : 0;
}
jint JNICALL AT_native_set_playback_rate(JNIEnv* env, jobject thiz, jint rate) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  // Playing at another rate needs a resampler; only the track's own is taken.
  return rate == c->track->config().sample_rate ? kSuccess : kErrorBadValue;
}

// PlaybackParams: speed and pitch need a time-stretcher, which a4l has not.
// What is set is ignored, and what is read back says so: speed 1, pitch 1.
void JNICALL AT_native_set_playback_params(JNIEnv*, jobject, jobject) {}
jobject JNICALL AT_native_get_playback_params(JNIEnv* env, jobject) {
  jclass cls = env->FindClass("android/media/PlaybackParams");
  if (cls == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  jobject params = nullptr;
  const jmethodID ctor = env->GetMethodID(cls, "<init>", "()V");
  const jmethodID speed =
      env->GetMethodID(cls, "setSpeed", "(F)Landroid/media/PlaybackParams;");
  const jmethodID pitch =
      env->GetMethodID(cls, "setPitch", "(F)Landroid/media/PlaybackParams;");
  if (ctor != nullptr && speed != nullptr && pitch != nullptr) {
    params = env->NewObject(cls, ctor);
    if (params != nullptr) {
      env->DeleteLocalRef(env->CallObjectMethod(params, speed, 1.0f));
      env->DeleteLocalRef(env->CallObjectMethod(params, pitch, 1.0f));
    }
  }
  if (env->ExceptionCheck())
    env->ExceptionClear();
  env->DeleteLocalRef(cls);
  return params;
}

jint JNICALL AT_native_set_marker_pos(JNIEnv* env, jobject thiz, jint v) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  c->marker = v;
  return kSuccess;
}
jint JNICALL AT_native_get_marker_pos(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  return c ? c->marker.load() : 0;
}
jint JNICALL AT_native_set_period(JNIEnv* env, jobject thiz, jint v) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  c->period = v;
  return kSuccess;
}
jint JNICALL AT_native_get_period(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  return c ? c->period.load() : 0;
}
jint JNICALL AT_native_set_start_threshold(JNIEnv* env, jobject thiz, jint v) {
  auto c = ctx_of(env, thiz);
  if (!c)
    return kErrorInvalidOperation;
  const jint capped = std::clamp(v, 1, c->track->config().capacity_frames);
  c->start_threshold = capped;
  return capped;
}
jint JNICALL AT_native_get_start_threshold(JNIEnv* env, jobject thiz) {
  auto c = ctx_of(env, thiz);
  return c ? c->start_threshold.load() : kErrorInvalidOperation;
}
jint JNICALL AT_native_set_loop(JNIEnv*, jobject, jint, jint, jint count) {
  return count == 0 ? kSuccess : kErrorInvalidOperation;  // looping: not built
}

// ── what a4l has no counterpart for ──────────────────────────────────────
// Effects, routing, volume shaping, presentations, dual mono: accepted where
// refusing would break an app that only sets a default, refused otherwise.

jint JNICALL AT_native_attachAuxEffect(JNIEnv*, jobject, jint effect) {
  return effect == 0 ? kSuccess : kErrorBadValue;  // 0 detaches
}
jint JNICALL AT_native_setAuxEffectSendLevel(JNIEnv*, jobject, jfloat) {
  return kSuccess;
}
jboolean JNICALL AT_native_setOutputDevice(JNIEnv*, jobject, jint) {
  return JNI_TRUE;
}
jint JNICALL AT_native_getRoutedDeviceId(JNIEnv*, jobject) {
  return 0;
}
void JNICALL AT_noop(JNIEnv*, jobject) {}
jint JNICALL AT_native_applyVolumeShaper(JNIEnv*, jobject, jobject, jobject) {
  return kErrorInvalidOperation;  // Java throws; a fade must not pass as done
}
jobject JNICALL AT_native_getVolumeShaperState(JNIEnv*, jobject, jint) {
  return nullptr;
}
jint JNICALL AT_native_setPresentation(JNIEnv*, jobject, jint, jint) {
  return kErrorInvalidOperation;
}
jint JNICALL AT_native_getPortId(JNIEnv*, jobject) {
  return 0;
}
void JNICALL AT_native_set_delay_padding(JNIEnv*, jobject, jint, jint) {}
jint JNICALL AT_native_set_ad_mix(JNIEnv*, jobject, jfloat) {
  return kErrorInvalidOperation;
}
jint JNICALL AT_native_get_ad_mix(JNIEnv*, jobject, jfloatArray) {
  return kErrorInvalidOperation;
}
jint JNICALL AT_native_set_dual_mono(JNIEnv*, jobject, jint) {
  return kErrorInvalidOperation;
}
jint JNICALL AT_native_get_dual_mono(JNIEnv*, jobject, jintArray) {
  return kErrorInvalidOperation;
}
void JNICALL AT_native_setLogSessionId(JNIEnv*, jobject, jstring) {}
void JNICALL AT_native_setPlayerIId(JNIEnv*, jobject, jint) {}
jobject JNICALL AT_native_getMetrics(JNIEnv*, jobject) {
  return nullptr;
}

// ── static natives ───────────────────────────────────────────────────────

jboolean JNICALL AT_native_is_direct_output_supported(JNIEnv*,
                                                      jclass,
                                                      jint,
                                                      jint,
                                                      jint,
                                                      jint,
                                                      jint,
                                                      jint,
                                                      jint) {
  return JNI_FALSE;  // no passthrough of encoded audio
}
jint JNICALL AT_native_get_output_sample_rate(JNIEnv*, jclass, jint) {
  return kOutputSampleRate;
}
// The smallest buffer, in bytes, for a streaming track of this description.
jint JNICALL AT_native_get_min_buff_size(JNIEnv*,
                                         jclass,
                                         jint rate,
                                         jint channel_count,
                                         jint encoding) {
  const int sample_bytes = bytes_per_sample(encoding);
  if (sample_bytes == 0 || channel_count < 1 || channel_count > 8 ||
      rate < 4000 || rate > 192000)
    return kErrorBadValue;
  return audio::Track::min_capacity_frames(rate) * channel_count * sample_bytes;
}

}  // namespace

void register_audio_track_natives(JNIEnv* env) {
  jclass cls = env->FindClass("android/media/AudioTrack");
  if (cls == nullptr) {
    env->ExceptionClear();
    return;
  }
  g_native_field = env->GetFieldID(cls, "mNativeTrackInJavaObj", "J");
  if (env->ExceptionCheck())
    env->ExceptionClear();
  g_stream_type = env->GetFieldID(cls, "mStreamType", "I");
  if (env->ExceptionCheck())
    env->ExceptionClear();
  if (g_native_field == nullptr) {
    // Not the framework's AudioTrack (a4l's own shim has its own natives).
    env->DeleteLocalRef(cls);
    return;
  }

#define AT(name, sig, fn)                            \
  {                                                  \
    const_cast<char*>(name), const_cast<char*>(sig), \
        reinterpret_cast<void*>(&fn)                 \
  }
  static const JNINativeMethod kMethods[] = {
      AT("native_setup",
         "(Ljava/lang/Object;Ljava/lang/Object;[IIIIII[IJZILjava/lang/Object;"
         "Ljava/lang/String;)I",
         AT_native_setup),
      AT("native_finalize", "()V", AT_native_finalize),
      AT("native_release", "()V", AT_native_release),
      AT("native_start", "()V", AT_native_start),
      AT("native_stop", "()V", AT_native_stop),
      AT("native_pause", "()V", AT_native_pause),
      AT("native_flush", "()V", AT_native_flush),
      AT("native_write_byte", "([BIIIZ)I", AT_native_write_byte),
      AT("native_write_short", "([SIIIZ)I", AT_native_write_short),
      AT("native_write_float", "([FIIIZ)I", AT_native_write_float),
      AT("native_write_native_bytes", "(Ljava/nio/ByteBuffer;IIIZ)I",
         AT_native_write_native_bytes),
      AT("native_reload_static", "()I", AT_native_reload_static),
      AT("native_get_buffer_size_frames", "()I", AT_native_get_buffer_frames),
      AT("native_set_buffer_size_frames", "(I)I", AT_native_set_buffer_frames),
      AT("native_get_buffer_capacity_frames", "()I",
         AT_native_get_buffer_frames),
      AT("native_setVolume", "(FF)V", AT_native_setVolume),
      AT("native_set_playback_rate", "(I)I", AT_native_set_playback_rate),
      AT("native_get_playback_rate", "()I", AT_native_get_playback_rate),
      AT("native_set_playback_params", "(Landroid/media/PlaybackParams;)V",
         AT_native_set_playback_params),
      AT("native_get_playback_params", "()Landroid/media/PlaybackParams;",
         AT_native_get_playback_params),
      AT("native_set_marker_pos", "(I)I", AT_native_set_marker_pos),
      AT("native_get_marker_pos", "()I", AT_native_get_marker_pos),
      AT("native_set_pos_update_period", "(I)I", AT_native_set_period),
      AT("native_get_pos_update_period", "()I", AT_native_get_period),
      AT("native_set_position", "(I)I", AT_native_set_position),
      AT("native_get_position", "()I", AT_native_get_position),
      AT("native_get_latency", "()I", AT_native_get_latency),
      AT("native_get_underrun_count", "()I", AT_native_get_underrun_count),
      AT("native_get_flags", "()I", AT_native_get_flags),
      AT("native_get_timestamp", "([J)I", AT_native_get_timestamp),
      AT("native_set_loop", "(III)I", AT_native_set_loop),
      AT("native_attachAuxEffect", "(I)I", AT_native_attachAuxEffect),
      AT("native_setAuxEffectSendLevel", "(F)I",
         AT_native_setAuxEffectSendLevel),
      AT("native_setOutputDevice", "(I)Z", AT_native_setOutputDevice),
      AT("native_getRoutedDeviceId", "()I", AT_native_getRoutedDeviceId),
      AT("native_enableDeviceCallback", "()V", AT_noop),
      AT("native_disableDeviceCallback", "()V", AT_noop),
      AT("native_applyVolumeShaper",
         "(Landroid/media/VolumeShaper$Configuration;"
         "Landroid/media/VolumeShaper$Operation;)I",
         AT_native_applyVolumeShaper),
      AT("native_getVolumeShaperState", "(I)Landroid/media/VolumeShaper$State;",
         AT_native_getVolumeShaperState),
      AT("native_setPresentation", "(II)I", AT_native_setPresentation),
      AT("native_getPortId", "()I", AT_native_getPortId),
      AT("native_set_delay_padding", "(II)V", AT_native_set_delay_padding),
      AT("native_set_audio_description_mix_level_db", "(F)I",
         AT_native_set_ad_mix),
      AT("native_get_audio_description_mix_level_db", "([F)I",
         AT_native_get_ad_mix),
      AT("native_set_dual_mono_mode", "(I)I", AT_native_set_dual_mono),
      AT("native_get_dual_mono_mode", "([I)I", AT_native_get_dual_mono),
      AT("native_setLogSessionId", "(Ljava/lang/String;)V",
         AT_native_setLogSessionId),
      AT("native_setStartThresholdInFrames", "(I)I",
         AT_native_set_start_threshold),
      AT("native_getStartThresholdInFrames", "()I",
         AT_native_get_start_threshold),
      AT("native_setPlayerIId", "(I)V", AT_native_setPlayerIId),
      AT("native_getMetrics", "()Landroid/os/PersistableBundle;",
         AT_native_getMetrics),
      AT("native_is_direct_output_supported", "(IIIIIII)Z",
         AT_native_is_direct_output_supported),
      AT("native_get_output_sample_rate", "(I)I",
         AT_native_get_output_sample_rate),
      AT("native_get_min_buff_size", "(III)I", AT_native_get_min_buff_size),
  };
#undef AT
  // One at a time: a framework of another release lacks or changes a few of
  // these, and one bad entry must not leave the rest unbound.
  int bound = 0;
  const int total = static_cast<int>(sizeof(kMethods) / sizeof(kMethods[0]));
  for (const JNINativeMethod& m : kMethods) {
    if (env->RegisterNatives(cls, &m, 1) == JNI_OK) {
      ++bound;
    } else {
      env->ExceptionClear();
      log::w(kTag, "AudioTrack.{}{} is not in this framework; left unbound",
             m.name, m.signature);
    }
  }
  env->DeleteLocalRef(cls);
  log::i(kTag, "android.media.AudioTrack natives registered ({} of {})", bound,
         total);
}

}  // namespace acl::runtime

// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.SoundPool public behavior (AOSP SDK docs) + its JNI
// method table (android_media_SoundPool.cpp method names/descriptors,
// Apache-2.0). Decode via FFmpeg (libav*, LGPL, dynamic link — permitted per
// plan.md policy); mix + output through acl's audio Sink (src/audio/audio.h).
// No GPL-3.0 sources. Clean-room native re-implementation: the real
// libsoundpool.so decodes via NDK MediaCodec and plays via
// AudioTrack/AudioFlinger, neither of which exists on the host — acl owns both
// ends (FFmpeg decode + PipeWire Sink).

#include <jni.h>

#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// FFmpeg headers are used for TYPES/structs only. The functions are resolved at
// runtime via dlopen(RTLD_LOCAL) (see g_ff below), never linked — so FFmpeg's
// transitive system OpenSSL stays out of the global symbol scope and cannot
// interpose on AOSP conscrypt's BoringSSL.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "audio/audio.h"
#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio.soundpool";
constexpr int kRate = 44100;     // SoundPool's default output rate
constexpr int kChannels = 2;     // mix everything to stereo
constexpr int kMixFrames = 512;  // per mixer iteration (~11.6 ms)

// ── FFmpeg resolved at runtime (RTLD_LOCAL) — see the OpenSSL-isolation note
// above.
struct Ff {
#define FF_FN(n) decltype(&::n) n = nullptr
  FF_FN(av_malloc);
  FF_FN(av_free);
  FF_FN(av_freep);
  FF_FN(av_samples_alloc);
  FF_FN(av_channel_layout_default);
  FF_FN(av_frame_alloc);
  FF_FN(av_frame_free);
  FF_FN(av_packet_alloc);
  FF_FN(av_packet_free);
  FF_FN(av_packet_unref);
  FF_FN(avio_alloc_context);
  FF_FN(avio_context_free);
  FF_FN(avformat_alloc_context);
  FF_FN(avformat_open_input);
  FF_FN(avformat_find_stream_info);
  FF_FN(avformat_close_input);
  FF_FN(av_find_best_stream);
  FF_FN(av_read_frame);
  FF_FN(avcodec_find_decoder);
  FF_FN(avcodec_alloc_context3);
  FF_FN(avcodec_parameters_to_context);
  FF_FN(avcodec_open2);
  FF_FN(avcodec_free_context);
  FF_FN(avcodec_send_packet);
  FF_FN(avcodec_receive_frame);
  FF_FN(swr_alloc_set_opts2);
  FF_FN(swr_init);
  FF_FN(swr_get_out_samples);
  FF_FN(swr_convert);
  FF_FN(swr_free);
#undef FF_FN
  bool ok = false;
};
Ff g_ff;
std::once_flag g_ff_once;

void* ff_open(const char* base, int maj) {
  // Prefer the runtime soname (always present); fall back to the -dev symlink.
  char v[64];
  std::snprintf(v, sizeof(v), "%s.%d", base, maj);
  if (void* h = ::dlopen(v, RTLD_NOW | RTLD_LOCAL))
    return h;
  return ::dlopen(base, RTLD_NOW | RTLD_LOCAL);
}

void load_ffmpeg() {
  // dlopen in dependency order; RTLD_LOCAL keeps their symbols (and system
  // OpenSSL) private.
  void* h[4] = {
      ff_open("libavutil.so", LIBAVUTIL_VERSION_MAJOR),
      ff_open("libswresample.so", LIBSWRESAMPLE_VERSION_MAJOR),
      ff_open("libavcodec.so", LIBAVCODEC_VERSION_MAJOR),
      ff_open("libavformat.so", LIBAVFORMAT_VERSION_MAJOR),
  };
  for (void* x : h)
    if (x == nullptr) {
      log::w(kTag, "FFmpeg dlopen failed; SoundPool decode off");
      return;
    }
  auto sym = [&](const char* n) -> void* {
    for (void* x : h)
      if (void* s = ::dlsym(x, n))
        return s;
    return nullptr;
  };
  bool all = true;
#define FF_BIND(n)                                        \
  do {                                                    \
    g_ff.n = reinterpret_cast<decltype(g_ff.n)>(sym(#n)); \
    if (!g_ff.n)                                          \
      all = false;                                        \
  } while (0)
  FF_BIND(av_malloc);
  FF_BIND(av_free);
  FF_BIND(av_freep);
  FF_BIND(av_samples_alloc);
  FF_BIND(av_channel_layout_default);
  FF_BIND(av_frame_alloc);
  FF_BIND(av_frame_free);
  FF_BIND(av_packet_alloc);
  FF_BIND(av_packet_free);
  FF_BIND(av_packet_unref);
  FF_BIND(avio_alloc_context);
  FF_BIND(avio_context_free);
  FF_BIND(avformat_alloc_context);
  FF_BIND(avformat_open_input);
  FF_BIND(avformat_find_stream_info);
  FF_BIND(avformat_close_input);
  FF_BIND(av_find_best_stream);
  FF_BIND(av_read_frame);
  FF_BIND(avcodec_find_decoder);
  FF_BIND(avcodec_alloc_context3);
  FF_BIND(avcodec_parameters_to_context);
  FF_BIND(avcodec_open2);
  FF_BIND(avcodec_free_context);
  FF_BIND(avcodec_send_packet);
  FF_BIND(avcodec_receive_frame);
  FF_BIND(swr_alloc_set_opts2);
  FF_BIND(swr_init);
  FF_BIND(swr_get_out_samples);
  FF_BIND(swr_convert);
  FF_BIND(swr_free);
#undef FF_BIND
  g_ff.ok = all;
  log::i(kTag, "FFmpeg decode {}",
         all ? "ready (dlopen RTLD_LOCAL)" : "unavailable (missing symbols)");
}

// ── FFmpeg decode: an in-memory encoded sample (OGG/WAV/MP3/…) → interleaved
// S16 stereo @ kRate.
struct MemBuf {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t pos;
};

int mem_read(void* opaque, std::uint8_t* buf, int want) {
  auto* m = static_cast<MemBuf*>(opaque);
  const std::size_t avail = m->size - m->pos;
  if (avail == 0)
    return AVERROR_EOF;
  const int n = static_cast<int>(std::min<std::size_t>(want, avail));
  std::memcpy(buf, m->data + m->pos, n);
  m->pos += n;
  return n;
}
std::int64_t mem_seek(void* opaque, std::int64_t off, int whence) {
  auto* m = static_cast<MemBuf*>(opaque);
  if (whence == AVSEEK_SIZE)
    return static_cast<std::int64_t>(m->size);
  std::size_t np = whence == SEEK_SET   ? static_cast<std::size_t>(off)
                   : whence == SEEK_CUR ? m->pos + off
                                        : m->size + off;
  if (np > m->size)
    return -1;
  m->pos = np;
  return static_cast<std::int64_t>(np);
}

bool decode_pcm(const std::uint8_t* data,
                std::size_t size,
                std::vector<std::int16_t>& out) {
  std::call_once(g_ff_once, load_ffmpeg);
  if (!g_ff.ok)
    return false;
  const int kIoBuf = 4096;
  auto* iobuf = static_cast<std::uint8_t*>(g_ff.av_malloc(kIoBuf));
  MemBuf mb{data, size, 0};
  AVIOContext* avio = g_ff.avio_alloc_context(iobuf, kIoBuf, 0, &mb, mem_read,
                                              nullptr, mem_seek);
  if (avio == nullptr) {
    g_ff.av_free(iobuf);
    return false;
  }
  AVFormatContext* fmt = g_ff.avformat_alloc_context();
  if (fmt == nullptr) {
    g_ff.av_free(avio->buffer);
    g_ff.avio_context_free(&avio);
    return false;
  }
  fmt->pb = avio;

  bool ok = false;
  AVCodecContext* cc = nullptr;
  SwrContext* swr = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  do {
    if (g_ff.avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0)
      break;
    if (g_ff.avformat_find_stream_info(fmt, nullptr) < 0)
      break;
    const int st =
        g_ff.av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (st < 0)
      break;
    AVCodecParameters* par = fmt->streams[st]->codecpar;
    const AVCodec* dec = g_ff.avcodec_find_decoder(par->codec_id);
    if (dec == nullptr)
      break;
    cc = g_ff.avcodec_alloc_context3(dec);
    if (cc == nullptr || g_ff.avcodec_parameters_to_context(cc, par) < 0)
      break;
    if (g_ff.avcodec_open2(cc, dec, nullptr) < 0)
      break;

    AVChannelLayout out_layout;
    g_ff.av_channel_layout_default(&out_layout, kChannels);
    if (g_ff.swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, kRate,
                                 &cc->ch_layout, cc->sample_fmt,
                                 cc->sample_rate, 0, nullptr) < 0 ||
        g_ff.swr_init(swr) < 0)
      break;

    pkt = g_ff.av_packet_alloc();
    frame = g_ff.av_frame_alloc();
    if (pkt == nullptr || frame == nullptr)
      break;
    while (g_ff.av_read_frame(fmt, pkt) >= 0) {
      if (pkt->stream_index == st && g_ff.avcodec_send_packet(cc, pkt) == 0) {
        while (g_ff.avcodec_receive_frame(cc, frame) == 0) {
          const int max_out = g_ff.swr_get_out_samples(swr, frame->nb_samples);
          std::uint8_t* obuf = nullptr;
          if (g_ff.av_samples_alloc(&obuf, nullptr, kChannels, max_out,
                                    AV_SAMPLE_FMT_S16, 0) < 0)
            continue;
          const int n = g_ff.swr_convert(
              swr, &obuf, max_out,
              const_cast<const std::uint8_t**>(frame->data), frame->nb_samples);
          if (n > 0) {
            auto* s = reinterpret_cast<std::int16_t*>(obuf);
            out.insert(out.end(), s,
                       s + static_cast<std::size_t>(n) * kChannels);
          }
          g_ff.av_freep(&obuf);
        }
      }
      g_ff.av_packet_unref(pkt);
    }
    // Flush the resampler tail.
    std::uint8_t* obuf = nullptr;
    if (g_ff.av_samples_alloc(&obuf, nullptr, kChannels, 4096,
                              AV_SAMPLE_FMT_S16, 0) >= 0) {
      const int n = g_ff.swr_convert(swr, &obuf, 4096, nullptr, 0);
      if (n > 0) {
        auto* s = reinterpret_cast<std::int16_t*>(obuf);
        out.insert(out.end(), s, s + static_cast<std::size_t>(n) * kChannels);
      }
      g_ff.av_freep(&obuf);
    }
    ok = !out.empty();
  } while (false);

  if (frame != nullptr)
    g_ff.av_frame_free(&frame);
  if (pkt != nullptr)
    g_ff.av_packet_free(&pkt);
  if (swr != nullptr)
    g_ff.swr_free(&swr);
  if (cc != nullptr)
    g_ff.avcodec_free_context(&cc);
  if (fmt->pb != nullptr) {
    g_ff.av_free(fmt->pb->buffer);
    g_ff.avio_context_free(&fmt->pb);
  }
  g_ff.avformat_close_input(&fmt);
  return ok;
}

// ── The engine: loaded sounds (id → PCM) + active voices + a mixer thread
// feeding the Sink.
struct Sound {
  std::vector<std::int16_t> pcm;
};  // interleaved S16 stereo @ kRate

struct Voice {
  int stream_id = 0;
  int sound_id = 0;
  double pos = 0;  // fractional frame index into the sound
  double rate =
      1.0;  // playback rate (pitch); pos advances by rate each output frame
  float lvol = 1.0f, rvol = 1.0f;
  int loop = 0;  // -1 = forever, 0 = once, N = repeat N extra times
  bool paused = false;
  bool active = false;
};

class SoundPool {
 public:
  explicit SoundPool(int max_streams) : max_streams_(std::max(1, max_streams)) {
    sink_ = make_sink();
    if (sink_ != nullptr && sink_->open(Format{kRate, kChannels})) {
      running_ = true;
      mixer_ = std::thread([this] { mix_loop(); });
    } else {
      log::w(kTag,
             "sink open failed; SoundPool audio disabled (app still runs)");
    }
  }

  ~SoundPool() {
    running_ = false;
    if (mixer_.joinable())
      mixer_.join();
    if (sink_ != nullptr)
      sink_->close();
  }

  int load(std::vector<std::int16_t>&& pcm) {
    std::lock_guard<std::mutex> lk(m_);
    const int id = next_sound_id_++;
    sounds_[id].pcm = std::move(pcm);
    return id;
  }

  int play(int sound_id, float lvol, float rvol, int loop, float rate) {
    std::lock_guard<std::mutex> lk(m_);
    if (sounds_.find(sound_id) == sounds_.end())
      return 0;
    const int sid = next_stream_id_++;
    Voice* v = alloc_voice();
    if (v == nullptr)
      return 0;
    *v = Voice{};
    v->stream_id = sid;
    v->sound_id = sound_id;
    v->lvol = clampv(lvol);
    v->rvol = clampv(rvol);
    v->loop = loop;
    v->rate = clamp_rate(rate);
    v->active = true;
    return sid;
  }

  void stop(int stream_id) {
    with_voice(stream_id, [](Voice& v) { v.active = false; });
  }
  void pause(int stream_id) {
    with_voice(stream_id, [](Voice& v) { v.paused = true; });
  }
  void resume(int stream_id) {
    with_voice(stream_id, [](Voice& v) { v.paused = false; });
  }
  void set_loop(int stream_id, int loop) {
    with_voice(stream_id, [loop](Voice& v) { v.loop = loop; });
  }
  void set_rate(int stream_id, float r) {
    with_voice(stream_id, [r](Voice& v) { v.rate = clamp_rate(r); });
  }
  void set_volume(int stream_id, float l, float r) {
    with_voice(stream_id, [l, r](Voice& v) {
      v.lvol = clampv(l);
      v.rvol = clampv(r);
    });
  }
  void auto_pause() {
    for_each([](Voice& v) { v.paused = true; });
  }
  void auto_resume() {
    for_each([](Voice& v) { v.paused = false; });
  }
  void set_muted(bool m) { muted_ = m; }
  void unload(int sound_id) {
    std::lock_guard<std::mutex> lk(m_);
    for (Voice& v : voices_)
      if (v.active && v.sound_id == sound_id)
        v.active = false;
    sounds_.erase(sound_id);
  }

 private:
  static float clampv(float v) { return std::clamp(v, 0.0f, 1.0f); }
  static double clamp_rate(float r) { return std::clamp<double>(r, 0.5, 2.0); }

  Voice* alloc_voice() {
    for (Voice& v : voices_)
      if (!v.active)
        return &v;
    if (static_cast<int>(voices_.size()) < max_streams_) {
      voices_.emplace_back();
      return &voices_.back();
    }
    return &voices_.front();  // voice-stealing: reuse the oldest slot
  }

  template <typename F>
  void with_voice(int stream_id, F fn) {
    std::lock_guard<std::mutex> lk(m_);
    for (Voice& v : voices_)
      if (v.active && v.stream_id == stream_id) {
        fn(v);
        return;
      }
  }
  template <typename F>
  void for_each(F fn) {
    std::lock_guard<std::mutex> lk(m_);
    for (Voice& v : voices_)
      if (v.active)
        fn(v);
  }

  void mix_loop() {
    std::vector<std::int32_t> acc(static_cast<std::size_t>(kMixFrames) *
                                  kChannels);
    std::vector<std::int16_t> out(static_cast<std::size_t>(kMixFrames) *
                                  kChannels);
    while (running_) {
      const auto t0 = std::chrono::steady_clock::now();
      std::fill(acc.begin(), acc.end(), 0);
      {
        std::lock_guard<std::mutex> lk(m_);
        const bool mute = muted_;
        for (Voice& v : voices_) {
          if (!v.active || v.paused)
            continue;
          auto it = sounds_.find(v.sound_id);
          if (it == sounds_.end()) {
            v.active = false;
            continue;
          }
          const std::vector<std::int16_t>& pcm = it->second.pcm;
          const std::size_t frames = pcm.size() / kChannels;
          if (frames == 0) {
            v.active = false;
            continue;
          }
          for (int i = 0; i < kMixFrames; ++i) {
            if (v.pos >= static_cast<double>(frames)) {
              if (v.loop == 0) {
                v.active = false;
                break;
              }
              if (v.loop > 0)
                --v.loop;
              v.pos -=
                  static_cast<double>(frames);  // wrap (keeps sub-sample phase)
            }
            const std::size_t idx = static_cast<std::size_t>(v.pos);
            const double frac = v.pos - static_cast<double>(idx);
            const std::size_t n1 = std::min(idx + 1, frames - 1);
            // linear interpolation per channel (rate/pitch support)
            const auto lerp = [&](int ch) -> std::int32_t {
              const double a = pcm[idx * kChannels + ch];
              const double b = pcm[n1 * kChannels + ch];
              return static_cast<std::int32_t>(a + (b - a) * frac);
            };
            if (!mute) {
              acc[i * kChannels + 0] +=
                  static_cast<std::int32_t>(lerp(0) * v.lvol);
              acc[i * kChannels + 1] +=
                  static_cast<std::int32_t>(lerp(1) * v.rvol);
            }
            v.pos += v.rate;
          }
        }
      }
      for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::int16_t>(std::clamp(acc[i], -32768, 32767));
      if (sink_ != nullptr)
        sink_->write(out.data(), static_cast<int>(out.size()));
      // Pace to real time (the null/counting sink returns instantly; the
      // PipeWire sink blocks — sleep only the remaining slice so both back-ends
      // run at ~kMixFrames/kRate per iteration).
      const auto budget =
          std::chrono::microseconds(kMixFrames * 1000000 / kRate);
      const auto spent = std::chrono::steady_clock::now() - t0;
      if (spent < budget)
        std::this_thread::sleep_for(budget - spent);
    }
  }

  const int max_streams_;
  std::unique_ptr<Sink> sink_;
  std::mutex m_;
  std::unordered_map<int, Sound> sounds_;
  std::vector<Voice> voices_;
  int next_sound_id_ = 1;
  int next_stream_id_ = 1;
  bool muted_ = false;
  std::atomic<bool> running_{false};
  std::thread mixer_;
};

// ── JNI seam. mNativeContext (a long field on SoundPool) holds the engine
// pointer.
jfieldID g_ctx_field = nullptr;  // SoundPool.mNativeContext (J)
jmethodID g_post_event =
    nullptr;  // SoundPool.postEventFromNative(IIILjava/lang/Object;)V
jfieldID g_fd_field = nullptr;  // FileDescriptor.descriptor (I)

SoundPool* engine(JNIEnv* env, jobject thiz) {
  if (g_ctx_field == nullptr)
    return nullptr;
  return reinterpret_cast<SoundPool*>(env->GetLongField(thiz, g_ctx_field));
}

jint JNICALL
SP_native_setup(JNIEnv* env, jobject thiz, jint max_streams, jobject, jstring) {
  auto* sp = new (std::nothrow) SoundPool(max_streams);
  if (sp == nullptr)
    return -1;
  env->SetLongField(thiz, g_ctx_field, reinterpret_cast<jlong>(sp));
  log::i(kTag, "SoundPool created (maxStreams={})", max_streams);
  return 0;
}

void JNICALL SP_native_release(JNIEnv* env, jobject thiz) {
  if (auto* sp = engine(env, thiz)) {
    delete sp;
    env->SetLongField(thiz, g_ctx_field, 0);
  }
}

jint JNICALL SP__load(JNIEnv* env,
                      jobject thiz,
                      jobject fd_obj,
                      jlong offset,
                      jlong length,
                      jint) {
  auto* sp = engine(env, thiz);
  if (sp == nullptr || fd_obj == nullptr || g_fd_field == nullptr ||
      length <= 0)
    return 0;
  const int fd = env->GetIntField(fd_obj, g_fd_field);
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(length));
  std::size_t got = 0;
  while (got < buf.size()) {
    const ssize_t n =
        ::pread(fd, buf.data() + got, buf.size() - got,
                static_cast<off_t>(offset) + static_cast<off_t>(got));
    if (n <= 0)
      break;
    got += static_cast<std::size_t>(n);
  }
  std::vector<std::int16_t> pcm;
  if (got == 0 || !decode_pcm(buf.data(), got, pcm)) {
    log::w(kTag, "sample decode failed (len={})",
           static_cast<long long>(length));
    return 0;
  }
  const int id = sp->load(std::move(pcm));
  // Tell the app the sample is ready (OnLoadCompleteListener.onLoadComplete):
  // msg=SAMPLE_LOADED(1), arg1=sampleID, arg2=status(0=OK). Apps commonly gate
  // playback on this callback.
  if (g_post_event != nullptr)
    env->CallVoidMethod(thiz, g_post_event, 1, id, 0, nullptr);
  return id;
}

jint JNICALL SP__play(JNIEnv* env,
                      jobject thiz,
                      jint sound_id,
                      jfloat lvol,
                      jfloat rvol,
                      jint,
                      jint loop,
                      jfloat rate) {
  auto* sp = engine(env, thiz);
  return sp != nullptr ? sp->play(sound_id, lvol, rvol, loop, rate) : 0;
}

void JNICALL SP_stop(JNIEnv* env, jobject thiz, jint stream) {
  if (auto* s = engine(env, thiz))
    s->stop(stream);
}
void JNICALL SP_pause(JNIEnv* env, jobject thiz, jint stream) {
  if (auto* s = engine(env, thiz))
    s->pause(stream);
}
void JNICALL SP_resume(JNIEnv* env, jobject thiz, jint stream) {
  if (auto* s = engine(env, thiz))
    s->resume(stream);
}
void JNICALL SP_autoPause(JNIEnv* env, jobject thiz) {
  if (auto* s = engine(env, thiz))
    s->auto_pause();
}
void JNICALL SP_autoResume(JNIEnv* env, jobject thiz) {
  if (auto* s = engine(env, thiz))
    s->auto_resume();
}
void JNICALL SP_setLoop(JNIEnv* env, jobject thiz, jint stream, jint loop) {
  if (auto* s = engine(env, thiz))
    s->set_loop(stream, loop);
}
void JNICALL SP_setRate(JNIEnv* env, jobject thiz, jint stream, jfloat r) {
  if (auto* s = engine(env, thiz))
    s->set_rate(stream, r);
}
void JNICALL
SP__setVolume(JNIEnv* env, jobject thiz, jint stream, jfloat l, jfloat r) {
  if (auto* s = engine(env, thiz))
    s->set_volume(stream, l, r);
}
void JNICALL SP__mute(JNIEnv* env, jobject thiz, jboolean m) {
  if (auto* s = engine(env, thiz))
    s->set_muted(m == JNI_TRUE);
}
void JNICALL SP_setPriority(JNIEnv*, jobject, jint, jint) {
}  // priority is advisory; ignore
jboolean JNICALL SP_unload(JNIEnv* env, jobject thiz, jint sound_id) {
  if (auto* s = engine(env, thiz)) {
    s->unload(sound_id);
    return JNI_TRUE;
  }
  return JNI_FALSE;
}

}  // namespace

// Register acl's SoundPool natives on the real framework's
// android.media.SoundPool (host-ART: the framework ships the class but its
// libsoundpool.so native is absent). Best-effort.
bool register_soundpool(JNIEnv* env) {
  jclass cls = env->FindClass("android/media/SoundPool");
  if (cls == nullptr) {
    env->ExceptionClear();
    return false;
  }
  g_ctx_field = env->GetFieldID(cls, "mNativeContext", "J");
  g_post_event =
      env->GetMethodID(cls, "postEventFromNative", "(IIILjava/lang/Object;)V");
  if (env->ExceptionCheck() == JNI_TRUE)
    env->ExceptionClear();
  if (jclass fdc = env->FindClass("java/io/FileDescriptor")) {
    g_fd_field = env->GetFieldID(fdc, "descriptor", "I");
    if (env->ExceptionCheck() == JNI_TRUE)
      env->ExceptionClear();
    env->DeleteLocalRef(fdc);
  }
  // JNINativeMethod declares name and signature as char*, not const char*, so a
  // string literal needs a cast to be stored in one. JNI only reads through
  // them. This matches the other registration tables in the tree.
  const JNINativeMethod m[] = {
      {const_cast<char*>("native_setup"),
       const_cast<char*>("(ILjava/lang/Object;Ljava/lang/String;)I"),
       reinterpret_cast<void*>(SP_native_setup)},
      {const_cast<char*>("native_release"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(SP_native_release)},
      {const_cast<char*>("_load"),
       const_cast<char*>("(Ljava/io/FileDescriptor;JJI)I"),
       reinterpret_cast<void*>(SP__load)},
      {const_cast<char*>("_play"), const_cast<char*>("(IFFIIF)I"),
       reinterpret_cast<void*>(SP__play)},
      {const_cast<char*>("_setVolume"), const_cast<char*>("(IFF)V"),
       reinterpret_cast<void*>(SP__setVolume)},
      {const_cast<char*>("_mute"), const_cast<char*>("(Z)V"),
       reinterpret_cast<void*>(SP__mute)},
      {const_cast<char*>("unload"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(SP_unload)},
      {const_cast<char*>("stop"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(SP_stop)},
      {const_cast<char*>("pause"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(SP_pause)},
      {const_cast<char*>("resume"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(SP_resume)},
      {const_cast<char*>("autoPause"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(SP_autoPause)},
      {const_cast<char*>("autoResume"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(SP_autoResume)},
      {const_cast<char*>("setLoop"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(SP_setLoop)},
      {const_cast<char*>("setRate"), const_cast<char*>("(IF)V"),
       reinterpret_cast<void*>(SP_setRate)},
      {const_cast<char*>("setPriority"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(SP_setPriority)},
  };
  const bool ok =
      g_ctx_field != nullptr &&
      env->RegisterNatives(cls, m, sizeof(m) / sizeof(m[0])) == JNI_OK;
  if (!ok)
    env->ExceptionClear();
  env->DeleteLocalRef(cls);
  log::i(kTag, "android.media.SoundPool natives {}",
         ok ? "registered" : "registration failed");
  return ok;
}

}  // namespace acl::audio

// This IS libsoundpool.so: android.media.SoundPool.<clinit> does
// System.loadLibrary("soundpool"), so ART loads this library and calls
// JNI_OnLoad, which binds acl's native implementations. It replaces the real
// (AudioFlinger/MediaCodec-backed) libsoundpool.so, which cannot run on the
// host.
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
      env == nullptr)
    return JNI_ERR;
  acl::audio::register_soundpool(env);
  return JNI_VERSION_1_6;
}

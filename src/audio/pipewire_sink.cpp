// SPDX-License-Identifier: Apache-2.0
// Provenance: PipeWire pw_stream API + examples (public, MIT) — pw_thread_loop,
// pw_stream_new_simple, spa_format_audio_raw_build; SPA audio format headers.
// No GPL-3.0 sources.
//
// The PipeWire AudioTrack backend: an OUTPUT pw_stream fed from a FIFO. The app's
// AudioTrack.write() pushes S16 PCM into the FIFO (write()); PipeWire pulls it in
// the realtime on_process callback. Built only when libpipewire is present
// (ACLCXX_HAVE_PIPEWIRE); make_sink() selects it via ACLCXX_AUDIO=pipewire.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "audio/audio.h"
#include "log/log.h"

namespace aclcxx::audio {
namespace {

constexpr std::string_view kTag = "aclcxx.audio";

class PipeWireSink final : public Sink {
 public:
  ~PipeWireSink() override { close(); }

  bool open(const Format& fmt) override {
    fmt_ = fmt;
    frames_ = 0;
    pw_init(nullptr, nullptr);
    inited_ = true;

    loop_ = pw_thread_loop_new("aclcxx-audio", nullptr);
    if (loop_ == nullptr) return false;

    static const pw_stream_events events = [] {
      pw_stream_events e{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.process = &PipeWireSink::on_process;
      return e;
    }();

    pw_thread_loop_lock(loop_);
    stream_ = pw_stream_new_simple(
        pw_thread_loop_get_loop(loop_), "aclcxx-audiotrack",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                          "Playback", PW_KEY_MEDIA_ROLE, "Music", nullptr),
        &events, this);
    if (stream_ == nullptr) {
      pw_thread_loop_unlock(loop_);
      return false;
    }

    std::uint8_t pod[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof(pod));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = static_cast<std::uint32_t>(fmt.sample_rate);
    info.channels = static_cast<std::uint32_t>(fmt.channels);
    const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    const int rc = pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    pw_thread_loop_unlock(loop_);
    if (rc < 0) return false;

    pw_thread_loop_start(loop_);
    log::i(kTag, "sink=pipewire open: {} Hz, {} ch, S16 (streaming to daemon)",
           fmt.sample_rate, fmt.channels);
    return true;
  }

  int write(const std::int16_t* pcm, int n_samples) override {
    if (n_samples <= 0 || stream_ == nullptr) return 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      fifo_.insert(fifo_.end(), pcm, pcm + n_samples);
    }
    frames_ += static_cast<std::uint64_t>(n_samples) /
               static_cast<std::uint64_t>(fmt_.channels > 0 ? fmt_.channels : 1);
    return n_samples;
  }

  void drain() override {
    // Best-effort: let the RT callback consume the FIFO before we tear down.
    for (int i = 0; i < 200; ++i) {  // up to ~1s
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (fifo_.empty()) break;
      }
      pw_thread_loop_lock(loop_);
      pw_thread_loop_timed_wait(loop_, 1);  // 1s cap per wait; woken by process
      pw_thread_loop_unlock(loop_);
    }
  }

  void close() override {
    if (closed_) return;  // idempotent: nativeRelease closes, then ~PipeWireSink
    closed_ = true;
    if (loop_ != nullptr) pw_thread_loop_stop(loop_);
    if (stream_ != nullptr) { pw_stream_destroy(stream_); stream_ = nullptr; }
    if (loop_ != nullptr) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
    if (inited_) { pw_deinit(); inited_ = false; }
    log::i(kTag, "sink=pipewire close: {} frames written", frames_);
  }

  std::uint64_t frames_written() const override { return frames_; }

 private:
  static void on_process(void* data) { static_cast<PipeWireSink*>(data)->process(); }

  void process() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (b == nullptr) return;
    spa_buffer* buf = b->buffer;
    auto* dst = static_cast<std::int16_t*>(buf->datas[0].data);
    if (dst == nullptr) { pw_stream_queue_buffer(stream_, b); return; }

    const int ch = fmt_.channels > 0 ? fmt_.channels : 1;
    int max_samples = static_cast<int>(buf->datas[0].maxsize / sizeof(std::int16_t));
    if (b->requested != 0) {
      max_samples = std::min(max_samples, static_cast<int>(b->requested) * ch);
    }

    int n = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      n = std::min(max_samples, static_cast<int>(fifo_.size()));
      for (int i = 0; i < n; ++i) dst[i] = fifo_.front(), fifo_.pop_front();
    }
    for (int i = n; i < max_samples; ++i) dst[i] = 0;  // pad underrun with silence
    const int filled = b->requested != 0 ? max_samples : n;

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = static_cast<std::int32_t>(ch * sizeof(std::int16_t));
    buf->datas[0].chunk->size =
        static_cast<std::uint32_t>(filled) * sizeof(std::int16_t);
    pw_stream_queue_buffer(stream_, b);
  }

  Format fmt_;
  pw_thread_loop* loop_ = nullptr;
  pw_stream* stream_ = nullptr;
  std::mutex mu_;
  std::deque<std::int16_t> fifo_;
  std::uint64_t frames_ = 0;
  bool inited_ = false;
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<Sink> make_pipewire_sink() {
  return std::make_unique<PipeWireSink>();
}

}  // namespace aclcxx::audio

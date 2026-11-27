// SPDX-License-Identifier: Apache-2.0
// Provenance: PipeWire pw_stream API + examples (public, MIT): pw_thread_loop,
// pw_stream_new_simple, spa_format_audio_raw_build; SPA audio format headers.
// After pipewire_sink.cpp. No GPL-3.0 sources.
//
// The PipeWire recording source: an INPUT pw_stream whose buffers are copied
// into a ring the reader drains. Built only when libpipewire is present
// (ACL_HAVE_PIPEWIRE); make_source() selects it with ACL_AUDIO=pipewire, behind
// the capture gate.
//
// The stream exists only between start() and stop(), so the desktop shows a
// microphone in use only while an app is recording. It connects to the
// desktop's default input unless ACL_AUDIO_CAPTURE_NODE names a node (its
// node.name or object.serial), and the session manager can move it after.
// PipeWire converts to the 16-bit format, the rate and the channels asked for.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "audio/audio.h"
#include "audio/audio_record.h"
#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio";

// A read that has waited this long for the device gets silence for the rest:
// a device that stops delivering does not hold the app's thread for good.
constexpr std::chrono::milliseconds kStarved{1000};

class PipeWireSource final : public Source {
 public:
  explicit PipeWireSource(std::string app_name)
      : app_name_(std::move(app_name)), fallback_(make_silent_source()) {}
  ~PipeWireSource() override { close(); }

  bool open(const Format& fmt, int capacity_frames) override {
    if (fmt.sample_rate <= 0 || fmt.channels <= 0 || capacity_frames <= 0)
      return false;
    if (!fallback_->open(fmt, capacity_frames))
      return false;
    fmt_ = fmt;
    // At least 100 ms: the graph delivers a period at a time, and a period
    // can be longer than the smallest buffer an app asks for.
    const auto frames = static_cast<std::size_t>(
        std::max(capacity_frames, fmt.sample_rate / 10));
    const std::lock_guard<std::mutex> lk(mu_);
    ring_.assign(frames * static_cast<std::size_t>(fmt.channels), 0);
    head_ = size_ = 0;
    return true;
  }

  void start() override {
    const std::lock_guard<std::mutex> ctl(ctl_);
    if (started_ || ring_.empty())
      return;
    {
      const std::lock_guard<std::mutex> lk(mu_);
      head_ = size_ = 0;
      running_ = true;
    }
    const bool silent = !connect();
    silent_.store(silent, std::memory_order_release);
    if (silent) {
      teardown();
      log::w(kTag, "source=pipewire: no input stream; recording silence");
      fallback_->start();
    }
    started_ = true;
  }

  void stop() override {
    const std::lock_guard<std::mutex> ctl(ctl_);
    if (!started_)
      return;
    started_ = false;
    {
      const std::lock_guard<std::mutex> lk(mu_);
      running_ = false;
    }
    wake_.notify_all();
    if (silent_.load(std::memory_order_acquire)) {
      fallback_->stop();
      return;
    }
    teardown();
    log::i(kTag, "source=pipewire stop: {} frames captured", frames_);
  }

  int read(std::int16_t* pcm, int frames, bool blocking) override {
    if (pcm == nullptr || frames <= 0)
      return 0;
    std::unique_lock<std::mutex> lk(mu_);
    if (silent_.load(std::memory_order_acquire)) {
      lk.unlock();
      return fallback_->read(pcm, frames, blocking);
    }
    const auto ch = static_cast<std::size_t>(fmt_.channels);
    const std::size_t want = static_cast<std::size_t>(frames) * ch;
    std::size_t got = 0;
    for (;;) {
      const std::size_t n = std::min(want - got, size_);
      for (std::size_t i = 0; i < n; ++i)
        pcm[got + i] = ring_[(head_ + i) % ring_.size()];
      head_ = (head_ + n) % ring_.size();
      size_ -= n;
      got += n;
      if (got == want || !blocking || !running_)
        break;
      if (!wake_.wait_for(lk, kStarved,
                          [this] { return size_ > 0 || !running_; })) {
        std::fill(pcm + got, pcm + want, std::int16_t{0});
        got = want;
        break;
      }
    }
    return static_cast<int>(got / ch);
  }

  void close() override { stop(); }

 private:
  static void on_process(void* data) {
    static_cast<PipeWireSource*>(data)->process();
  }

  void process() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (b == nullptr)
      return;
    const spa_data& d = b->buffer->datas[0];
    if (d.data != nullptr && d.chunk != nullptr) {
      const std::uint32_t off = std::min(d.chunk->offset, d.maxsize);
      const std::uint32_t bytes = std::min(d.chunk->size, d.maxsize - off);
      const auto ch = static_cast<std::size_t>(fmt_.channels);
      std::size_t n = bytes / sizeof(std::int16_t);
      n -= n % ch;
      const auto* src = reinterpret_cast<const std::uint8_t*>(d.data) + off;
      const std::lock_guard<std::mutex> lk(mu_);
      if (running_ && n > 0) {
        // More than the ring holds: the newest part of it.
        if (n > ring_.size()) {
          src += (n - ring_.size()) * sizeof(std::int16_t);
          n = ring_.size();
        }
        // A reader that is late loses the oldest frames.
        const std::size_t room = ring_.size() - size_;
        if (n > room) {
          const std::size_t drop = n - room;
          head_ = (head_ + drop) % ring_.size();
          size_ -= drop;
        }
        const std::size_t tail = (head_ + size_) % ring_.size();
        const std::size_t first = std::min(n, ring_.size() - tail);
        std::memcpy(&ring_[tail], src, first * sizeof(std::int16_t));
        std::memcpy(ring_.data(), src + first * sizeof(std::int16_t),
                    (n - first) * sizeof(std::int16_t));
        size_ += n;
        frames_ += n / ch;
        wake_.notify_all();
      }
    }
    pw_stream_queue_buffer(stream_, b);
  }

  // The stream, connected and seen to reach the daemon. False with whatever
  // was made left for teardown().
  bool connect() {
    pw_init(nullptr, nullptr);
    inited_ = true;
    frames_ = 0;
    loop_ = pw_thread_loop_new("acl-capture", nullptr);
    if (loop_ == nullptr)
      return false;

    static const pw_stream_events events = [] {
      pw_stream_events e{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.process = &PipeWireSource::on_process;
      return e;
    }();

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                          "Capture", PW_KEY_APP_NAME, app_name_.c_str(),
                          PW_KEY_NODE_DESCRIPTION, app_name_.c_str(), nullptr);
    if (props == nullptr)
      return false;
    const char* node = std::getenv("ACL_AUDIO_CAPTURE_NODE");
    if (node != nullptr && node[0] != '\0') {
      pw_properties_set(props, PW_KEY_TARGET_OBJECT, node);
      // A node that is named and absent is no input, not the default one.
      pw_properties_set(props, "node.dont-fallback", "true");
    }

    pw_thread_loop_lock(loop_);
    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
                                   "acl-audiorecord", props, &events, this);
    if (stream_ == nullptr) {
      pw_thread_loop_unlock(loop_);
      return false;
    }

    std::uint8_t pod[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof(pod));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = static_cast<std::uint32_t>(fmt_.sample_rate);
    info.channels = static_cast<std::uint32_t>(fmt_.channels);
    const spa_pod* params[1] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    const int rc = pw_stream_connect(
        stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    pw_thread_loop_unlock(loop_);
    if (rc < 0)
      return false;

    pw_thread_loop_start(loop_);
    loop_started_ = true;

    // Bounded, as the sink's: an unreachable daemon leaves the stream
    // connecting for good.
    for (int i = 0; i < 50; ++i) {  // ~500 ms
      pw_thread_loop_lock(loop_);
      const pw_stream_state st = pw_stream_get_state(stream_, nullptr);
      pw_thread_loop_unlock(loop_);
      if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING) {
        log::i(kTag, "source=pipewire start: {} Hz, {} ch, S16, node {}",
               fmt_.sample_rate, fmt_.channels,
               node != nullptr && node[0] != '\0' ? node : "default");
        return true;
      }
      if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED)
        break;
      const timespec ts{0, 10 * 1000 * 1000};
      nanosleep(&ts, nullptr);
    }
    return false;
  }

  void teardown() {
    if (loop_ != nullptr && loop_started_)
      pw_thread_loop_stop(loop_);
    loop_started_ = false;
    if (stream_ != nullptr) {
      pw_stream_destroy(stream_);
      stream_ = nullptr;
    }
    if (loop_ != nullptr) {
      pw_thread_loop_destroy(loop_);
      loop_ = nullptr;
    }
    if (inited_) {
      pw_deinit();
      inited_ = false;
    }
  }

  const std::string app_name_;
  std::unique_ptr<Source> fallback_;  // silence, when no stream can be made
  Format fmt_;

  std::mutex ctl_;  // start, stop and what they make
  bool started_ = false;
  std::atomic<bool> silent_{false};  // the silent source stands in
  pw_thread_loop* loop_ = nullptr;
  pw_stream* stream_ = nullptr;
  bool inited_ = false;
  bool loop_started_ = false;

  std::mutex mu_;  // the ring
  std::condition_variable wake_;
  std::vector<std::int16_t> ring_;  // interleaved samples
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  bool running_ = false;
  std::uint64_t frames_ = 0;
};

}  // namespace

std::unique_ptr<Source> make_pipewire_source(std::string app_name) {
  return std::make_unique<PipeWireSource>(
      app_name.empty() ? std::string{"a4l"} : std::move(app_name));
}

}  // namespace acl::audio

// SPDX-License-Identifier: Apache-2.0
// Provenance: the ALSA PCM API (alsa-lib, LGPL-2.1, linked dynamically):
// snd_pcm_open, snd_pcm_set_params, snd_pcm_readi, snd_pcm_recover. After
// pipewire_source.cpp. No GPL-3.0 sources.
//
// The ALSA recording source, for a system with no PipeWire: a capture PCM read
// by a thread of its own into a ring the reader drains. Built only when
// alsa-lib is present (ACL_HAVE_ALSA); make_source() chooses it, behind the
// capture gate.
//
// The PCM is open only between start() and stop(), so the device is held only
// while an app is recording. It is "default" unless ACL_ALSA_DEVICE names
// another. With no device to open, a recording hears silence.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <alsa/asoundlib.h>

#include "audio/alsa_device.h"
#include "audio/audio.h"
#include "audio/audio_record.h"
#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio";

constexpr unsigned kDeviceLatencyUs = 100'000;
constexpr int kWaitMs = 200;
// A blocking read with nothing arriving gives silence after this long, as the
// PipeWire source's does.
constexpr std::chrono::milliseconds kStarved{1000};

class AlsaSource final : public Source {
 public:
  AlsaSource() : fallback_(make_silent_source()) {}
  ~AlsaSource() override { close(); }

  bool open(const Format& fmt, int capacity_frames) override {
    if (fmt.sample_rate <= 0 || fmt.channels <= 0 || capacity_frames <= 0)
      return false;
    if (!fallback_->open(fmt, capacity_frames))
      return false;
    fmt_ = fmt;
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
      log::w(kTag, "source=alsa: no capture device; recording silence");
      fallback_->start();
    } else {
      thread_ = std::thread([this] { run(); });
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
    if (thread_.joinable())
      thread_.join();
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    log::i(kTag, "source=alsa stop: {} frames captured", frames_);
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
  bool connect() {
    frames_ = 0;
    const std::string device = alsa_device_name();
    int rc = snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
      log::w(kTag, "source=alsa: cannot open '{}' ({})", device,
             snd_strerror(rc));
      pcm_ = nullptr;
      return false;
    }
    rc = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            static_cast<unsigned>(fmt_.channels),
                            static_cast<unsigned>(fmt_.sample_rate),
                            1 /* let alsa-lib resample */, kDeviceLatencyUs);
    if (rc < 0) {
      log::w(kTag, "source=alsa: '{}' refused {} Hz, {} ch, S16 ({})", device,
             fmt_.sample_rate, fmt_.channels, snd_strerror(rc));
      snd_pcm_close(pcm_);
      pcm_ = nullptr;
      return false;
    }
    log::i(kTag, "source=alsa start: {} Hz, {} ch, S16, device '{}'",
           fmt_.sample_rate, fmt_.channels, device);
    return true;
  }

  // Appends to the ring, dropping the oldest samples when it is full.
  void keep(const std::int16_t* src, std::size_t n) {
    const auto ch = static_cast<std::size_t>(fmt_.channels);
    const std::lock_guard<std::mutex> lk(mu_);
    if (!running_ || n == 0)
      return;
    if (n > ring_.size()) {
      src += n - ring_.size();
      n = ring_.size();
    }
    const std::size_t room = ring_.size() - size_;
    if (n > room) {
      const std::size_t drop = n - room;
      head_ = (head_ + drop) % ring_.size();
      size_ -= drop;
    }
    const std::size_t tail = (head_ + size_) % ring_.size();
    const std::size_t first = std::min(n, ring_.size() - tail);
    std::memcpy(&ring_[tail], src, first * sizeof(std::int16_t));
    std::memcpy(ring_.data(), src + first, (n - first) * sizeof(std::int16_t));
    size_ += n;
    frames_ += n / ch;
    wake_.notify_all();
  }

  void run() {
    const auto ch = static_cast<std::size_t>(fmt_.channels);
    const std::size_t period = std::max<std::size_t>(
        1, static_cast<std::size_t>(fmt_.sample_rate) / 50);  // 20 ms
    std::vector<std::int16_t> chunk(period * ch);
    snd_pcm_start(pcm_);
    for (;;) {
      {
        const std::lock_guard<std::mutex> lk(mu_);
        if (!running_)
          return;
      }
      int rc = snd_pcm_wait(pcm_, kWaitMs);
      if (rc == 0)
        continue;  // nothing yet; look at running_ again
      snd_pcm_sframes_t n = rc;
      if (rc > 0)
        n = snd_pcm_readi(pcm_, chunk.data(),
                          static_cast<snd_pcm_uframes_t>(period));
      if (n == -EAGAIN)
        continue;
      if (n < 0) {
        // An overrun (the reader of the device was late) or a suspend.
        rc = snd_pcm_recover(pcm_, static_cast<int>(n), 1 /* silent */);
        if (rc < 0) {
          // The reader gets silence from its own timeout from here on.
          log::w(kTag, "source=alsa: device lost ({})", snd_strerror(rc));
          return;
        }
        snd_pcm_start(pcm_);
        continue;
      }
      keep(chunk.data(), static_cast<std::size_t>(n) * ch);
    }
  }

  std::unique_ptr<Source> fallback_;
  Format fmt_;
  std::mutex ctl_;  // start()/stop(), which open and close the device
  std::mutex mu_;   // the ring and running_
  std::condition_variable wake_;
  std::vector<std::int16_t> ring_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  bool running_ = false;
  bool started_ = false;
  std::atomic<bool> silent_{false};
  snd_pcm_t* pcm_ = nullptr;
  std::thread thread_;
  std::uint64_t frames_ = 0;
};

}  // namespace

std::unique_ptr<Source> make_alsa_source() {
  return std::make_unique<AlsaSource>();
}

}  // namespace acl::audio

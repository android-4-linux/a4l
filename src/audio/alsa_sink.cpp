// SPDX-License-Identifier: Apache-2.0
// Provenance: the ALSA PCM API (alsa-lib, LGPL-2.1, linked dynamically):
// snd_pcm_open, snd_pcm_set_params, snd_pcm_writei, snd_pcm_recover,
// snd_pcm_delay. After pipewire_sink.cpp. No GPL-3.0 sources.
//
// The ALSA AudioTrack backend, for a system with no PipeWire: a playback PCM
// fed from a FIFO by a thread of its own. write() never blocks the app; the
// thread hands the device a period at a time. Built only when alsa-lib is
// present (ACL_HAVE_ALSA); see make_device_sink() for when it is chosen.
//
// The device is "default" unless ACL_ALSA_DEVICE names another. alsa-lib
// converts to the rate and channels asked for where the device differs (its
// "plug" layer, which "default" has).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <alsa/asoundlib.h>

#include "audio/alsa_device.h"
#include "audio/audio.h"
#include "log/log.h"

namespace acl::audio {

std::string alsa_device_name() {
  const char* name = std::getenv("ACL_ALSA_DEVICE");
  return name != nullptr && name[0] != '\0' ? name : "default";
}

namespace {

constexpr std::string_view kTag = "acl.audio";

// What the device is asked to hold: enough that a late thread does not
// underrun it, little enough that a player's picture stays with its sound.
constexpr unsigned kDeviceLatencyUs = 100'000;
// How long the thread waits for the device to take more before it looks at
// whether it has been told to stop.
constexpr int kWaitMs = 200;

class AlsaSink final : public Sink {
 public:
  ~AlsaSink() override { close(); }

  bool open(const Format& fmt) override {
    if (fmt.sample_rate <= 0 || fmt.channels <= 0 || pcm_ != nullptr)
      return false;
    fmt_ = fmt;
    frames_ = 0;
    const std::string device = alsa_device_name();
    // Not blocking: a full device answers EAGAIN and the thread waits for room
    // itself, where it can also see that it has been told to stop.
    int rc = snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_PLAYBACK,
                          SND_PCM_NONBLOCK);
    if (rc < 0) {
      log::w(kTag, "sink=alsa: cannot open '{}' ({})", device,
             snd_strerror(rc));
      pcm_ = nullptr;
      return false;
    }
    rc = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            static_cast<unsigned>(fmt.channels),
                            static_cast<unsigned>(fmt.sample_rate),
                            1 /* let alsa-lib resample */, kDeviceLatencyUs);
    if (rc < 0) {
      log::w(kTag, "sink=alsa: '{}' refused {} Hz, {} ch, S16 ({})", device,
             fmt.sample_rate, fmt.channels, snd_strerror(rc));
      snd_pcm_close(pcm_);
      pcm_ = nullptr;
      return false;
    }
    // set_params leaves the device to start once its buffer is full. A sound
    // shorter than the buffer would then never start, so start at the first
    // frame; the FIFO behind it fills faster than the device plays.
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(pcm_, sw) < 0 ||
        snd_pcm_sw_params_set_start_threshold(pcm_, sw, 1) < 0 ||
        snd_pcm_sw_params(pcm_, sw) < 0) {
      log::w(kTag, "sink=alsa: '{}' would not take a start threshold", device);
      snd_pcm_close(pcm_);
      pcm_ = nullptr;
      return false;
    }
    closed_ = false;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    log::i(kTag, "sink=alsa open: {} Hz, {} ch, S16, device '{}'",
           fmt.sample_rate, fmt.channels, device);
    return true;
  }

  int write(const std::int16_t* pcm, int n_samples) override {
    if (n_samples <= 0 || pcm_ == nullptr)
      return 0;
    {
      const std::lock_guard<std::mutex> lk(mu_);
      fifo_.insert(fifo_.end(), pcm, pcm + n_samples);
    }
    more_.notify_one();
    frames_ += static_cast<std::uint64_t>(n_samples) /
               static_cast<std::uint64_t>(fmt_.channels);
    return n_samples;
  }

  void drain() override {
    // Bounded, as the PipeWire sink's is: a device that has stopped taking
    // samples must not hold the app.
    if (pcm_ == nullptr)
      return;
    std::unique_lock<std::mutex> lk(mu_);
    drain_ = true;
    more_.notify_one();
    drained_.wait_for(lk, std::chrono::seconds(3), [this] { return !drain_; });
    drain_ = false;
  }

  void close() override {
    if (closed_)
      return;  // nativeRelease closes, then the destructor
    closed_ = true;
    {
      const std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    more_.notify_one();
    if (thread_.joinable())
      thread_.join();
    if (pcm_ != nullptr) {
      snd_pcm_close(pcm_);
      pcm_ = nullptr;
    }
    log::i(kTag, "sink=alsa close: {} frames written", frames_);
  }

  std::uint64_t frames_written() const override { return frames_; }
  std::uint64_t frames_pending() const override {
    const std::lock_guard<std::mutex> lk(mu_);
    return fifo_.size() / static_cast<std::size_t>(fmt_.channels);
  }
  std::int64_t latency_ns() const override {
    return latency_ns_.load(std::memory_order_relaxed);
  }

 private:
  // The frames the device holds and has not yet sounded.
  void note_latency() {
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(pcm_, &delay) < 0 || delay < 0)
      return;
    const std::int64_t ns =
        static_cast<std::int64_t>(delay) * 1'000'000'000 / fmt_.sample_rate;
    latency_ns_.store(std::min(ns, kMaxLatencyNs), std::memory_order_relaxed);
  }

  // Waits for the device to sound what it holds: until it reports nothing
  // left or stops running (it underruns once it is empty), and no longer than
  // twice what it was asked to hold. Not snd_pcm_drain(), which held a
  // Raspberry Pi 4's bcm2835 device for 3 s after a half-second tone.
  void wait_played() {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(2 * kDeviceLatencyUs);
    while (std::chrono::steady_clock::now() < deadline) {
      snd_pcm_sframes_t delay = 0;
      if (snd_pcm_state(pcm_) != SND_PCM_STATE_RUNNING ||
          snd_pcm_delay(pcm_, &delay) < 0 || delay <= 0)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // False when the device is gone for good.
  bool play(const std::vector<std::int16_t>& chunk) {
    const auto ch = static_cast<std::size_t>(fmt_.channels);
    std::size_t done = 0;
    const std::size_t frames = chunk.size() / ch;
    while (done < frames) {
      {
        const std::lock_guard<std::mutex> lk(mu_);
        if (stop_)
          return true;
      }
      const snd_pcm_sframes_t n =
          snd_pcm_writei(pcm_, chunk.data() + done * ch,
                         static_cast<snd_pcm_uframes_t>(frames - done));
      if (n == -EAGAIN) {
        // Full. A device that is full and not running will never make room.
        if (snd_pcm_state(pcm_) == SND_PCM_STATE_PREPARED)
          snd_pcm_start(pcm_);
        snd_pcm_wait(pcm_, kWaitMs);  // an error here shows at the next write
        continue;
      }
      if (n < 0) {
        // An underrun (the FIFO ran dry) or a suspend: start the device again.
        const int rc = snd_pcm_recover(pcm_, static_cast<int>(n), 1);
        if (rc < 0) {
          log::w(kTag, "sink=alsa: device lost ({})", snd_strerror(rc));
          return false;
        }
        continue;
      }
      done += static_cast<std::size_t>(n);
    }
    note_latency();
    return true;
  }

  void run() {
    const auto ch = static_cast<std::size_t>(fmt_.channels);
    // 20 ms at a time: short enough to stop promptly, long enough not to
    // wake for every few samples.
    const std::size_t period =
        std::max<std::size_t>(1,
                              static_cast<std::size_t>(fmt_.sample_rate) / 50) *
        ch;
    std::vector<std::int16_t> chunk;
    bool lost = false;
    for (;;) {
      chunk.clear();
      {
        std::unique_lock<std::mutex> lk(mu_);
        more_.wait(lk, [this] { return stop_ || drain_ || !fifo_.empty(); });
        if (stop_)
          return;
        const std::size_t n = std::min(period, fifo_.size());
        chunk.assign(fifo_.begin(),
                     fifo_.begin() + static_cast<std::ptrdiff_t>(n));
        fifo_.erase(fifo_.begin(),
                    fifo_.begin() + static_cast<std::ptrdiff_t>(n));
      }
      // Samples for a device that is gone are dropped, so the FIFO does not
      // grow and the app's writes go on being counted.
      if (!chunk.empty() && !lost)
        lost = !play(chunk);
      bool drain = false;
      {
        const std::lock_guard<std::mutex> lk(mu_);
        drain = drain_ && fifo_.empty();
      }
      if (!drain)
        continue;
      if (!lost)
        wait_played();
      const std::lock_guard<std::mutex> lk(mu_);
      drain_ = false;
      drained_.notify_all();
    }
  }

  Format fmt_;
  snd_pcm_t* pcm_ = nullptr;
  std::thread thread_;
  mutable std::mutex mu_;
  std::condition_variable more_;
  std::condition_variable drained_;
  std::deque<std::int16_t> fifo_;
  bool stop_ = false;
  bool drain_ = false;
  std::uint64_t frames_ = 0;
  // A device that reports more than this is reporting a fault.
  static constexpr std::int64_t kMaxLatencyNs = 2'000'000'000;
  std::atomic<std::int64_t> latency_ns_{-1};  // -1: not yet read
  bool closed_ = true;
};

}  // namespace

std::unique_ptr<Sink> make_alsa_sink() {
  return std::make_unique<AlsaSink>();
}

}  // namespace acl::audio

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; Android SDK documentation for
// android.media.AudioRecord. No GPL-3.0 sources.
#include "audio/audio_record.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include "acl_config.h"
#if ACL_HAVE_ALSA
#include "audio/alsa_device.h"
#endif
#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio.record";

constexpr int kMinRate = 4000;
constexpr int kMaxRate = 192000;
constexpr int kMaxChannels = 8;
// Ten seconds at the top rate: more is a mistake, not a buffer.
constexpr int kMaxCapacityFrames = kMaxRate * 10;

using Clock = std::chrono::steady_clock;

// Frames that `elapsed` amounts to at `rate`.
std::uint64_t frames_in(Clock::duration elapsed, int rate) {
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  if (ns <= 0)
    return 0;
  const auto u = static_cast<std::uint64_t>(ns);
  const auto r = static_cast<std::uint64_t>(rate);
  // Whole seconds apart from the rest, so the product stays in 64 bits.
  return u / 1'000'000'000U * r + u % 1'000'000'000U * r / 1'000'000'000U;
}

class SilentSource final : public Source {
 public:
  bool open(const Format& fmt, int capacity_frames) override {
    if (fmt.sample_rate <= 0 || fmt.channels <= 0 || capacity_frames <= 0)
      return false;
    fmt_ = fmt;
    capacity_ = static_cast<std::uint64_t>(capacity_frames);
    return true;
  }

  void start() override {
    std::lock_guard<std::mutex> lock(mu_);
    if (started_)
      return;
    started_ = true;
    since_ = Clock::now();
    taken_ = 0;
    left_ = 0;
  }

  void stop() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!started_)
        return;
      // What had come in by now stays readable.
      left_ = ready_locked(Clock::now());
      started_ = false;
    }
    wake_.notify_all();
  }

  int read(std::int16_t* pcm, int frames, bool blocking) override {
    if (pcm == nullptr || frames <= 0)
      return 0;
    const auto want = static_cast<std::uint64_t>(frames);
    std::uint64_t got = 0;
    std::unique_lock<std::mutex> lock(mu_);
    while (got < want) {
      if (!started_) {
        const std::uint64_t n = std::min(want - got, left_);
        left_ -= n;
        got += n;
        break;
      }
      const Clock::time_point now = Clock::now();
      const std::uint64_t n = std::min(want - got, ready_locked(now));
      taken_ += n;
      got += n;
      if (got == want || !blocking)
        break;
      // Sleep until the rest is due. stop() wakes it sooner.
      const std::uint64_t due = taken_ + (want - got);
      const auto wait = std::chrono::nanoseconds(
          due / static_cast<std::uint64_t>(fmt_.sample_rate) * 1'000'000'000U +
          due % static_cast<std::uint64_t>(fmt_.sample_rate) * 1'000'000'000U /
              static_cast<std::uint64_t>(fmt_.sample_rate) +
          1'000'000U);
      wake_.wait_until(lock, since_ + wait);
    }
    lock.unlock();
    std::memset(pcm, 0,
                static_cast<std::size_t>(got) *
                    static_cast<std::size_t>(fmt_.channels) *
                    sizeof(std::int16_t));
    return static_cast<int>(got);
  }

  void close() override { stop(); }

 private:
  // Frames come in by the clock and wait for the reader, up to the capacity.
  // A reader further behind than that has lost the oldest.
  std::uint64_t ready_locked(Clock::time_point now) {
    const std::uint64_t in = frames_in(now - since_, fmt_.sample_rate);
    if (in > taken_ + capacity_)
      taken_ = in - capacity_;
    return in - taken_;
  }

  Format fmt_;
  std::uint64_t capacity_ = 0;
  std::mutex mu_;
  std::condition_variable wake_;
  bool started_ = false;
  Clock::time_point since_;
  std::uint64_t taken_ = 0;  // frames read or dropped since start
  std::uint64_t left_ = 0;   // frames still readable after stop
};

}  // namespace

std::unique_ptr<Source> make_silent_source() {
  return std::make_unique<SilentSource>();
}

namespace {

// Both sources are opened; start() picks the one the recording reads until
// the next start().
class GatedSource final : public Source {
 public:
  GatedSource(std::unique_ptr<Source> device, std::function<bool()> allowed)
      : device_(std::move(device)),
        silent_(make_silent_source()),
        allowed_(std::move(allowed)),
        active_(silent_.get()) {}

  bool open(const Format& fmt, int capacity_frames) override {
    if (!silent_->open(fmt, capacity_frames))
      return false;
    // A device that cannot be opened is a recording of silence, not a
    // recorder that fails.
    if (device_ != nullptr && !device_->open(fmt, capacity_frames))
      device_.reset();
    return true;
  }

  void start() override {
    const std::lock_guard<std::mutex> lock(mu_);
    if (started_)
      return;
    started_ = true;
    Source* pick = silent_.get();
    if (device_ != nullptr && allowed_ && allowed_())
      pick = device_.get();
    active_.store(pick, std::memory_order_release);
    pick->start();
  }

  void stop() override {
    const std::lock_guard<std::mutex> lock(mu_);
    if (!started_)
      return;
    started_ = false;
    active_.load(std::memory_order_acquire)->stop();
  }

  // Not under mu_: a blocked read must not hold up the stop that ends it.
  int read(std::int16_t* pcm, int frames, bool blocking) override {
    return active_.load(std::memory_order_acquire)->read(pcm, frames, blocking);
  }

  void close() override {
    stop();
    silent_->close();
    if (device_ != nullptr)
      device_->close();
  }

 private:
  std::unique_ptr<Source> device_;
  const std::unique_ptr<Source> silent_;
  const std::function<bool()> allowed_;
  std::mutex mu_;  // start and stop
  bool started_ = false;
  std::atomic<Source*> active_;
};

std::mutex g_capture_mu;
Capture g_capture;  // guarded by g_capture_mu

}  // namespace

std::unique_ptr<Source> make_gated_source(std::unique_ptr<Source> device,
                                          std::function<bool()> allowed) {
  return std::make_unique<GatedSource>(std::move(device), std::move(allowed));
}

void set_capture(Capture capture) {
  const std::lock_guard<std::mutex> lock(g_capture_mu);
  g_capture = std::move(capture);
}

std::unique_ptr<Source> make_source() {
  const DeviceBackend backend = device_backend();
  if (backend != DeviceBackend::kNone) {
    Capture capture;
    {
      const std::lock_guard<std::mutex> lock(g_capture_mu);
      capture = g_capture;
    }
    // No gate, no device: nothing is made that could hear one.
    if (capture.allowed) {
#if ACL_HAVE_PIPEWIRE
      if (backend == DeviceBackend::kPipeWire)
        return make_gated_source(make_pipewire_source(capture.app_name),
                                 std::move(capture.allowed));
#endif
#if ACL_HAVE_ALSA
      if (backend == DeviceBackend::kAlsa)
        return make_gated_source(make_alsa_source(),
                                 std::move(capture.allowed));
#endif
    }
  }
  return make_silent_source();
}

bool Recorder::valid(const Config& c) noexcept {
  return c.sample_rate >= kMinRate && c.sample_rate <= kMaxRate &&
         c.channels >= 1 && c.channels <= kMaxChannels &&
         c.capacity_frames >= 1 && c.capacity_frames <= kMaxCapacityFrames;
}

int Recorder::min_capacity_frames(int sample_rate) noexcept {
  if (sample_rate < kMinRate || sample_rate > kMaxRate)
    return 0;
  return sample_rate / 1000 * 40 + sample_rate % 1000 * 40 / 1000;
}

Recorder::Recorder(const Config& config, std::unique_ptr<Source> source)
    : config_(config), source_(std::move(source)) {
  if (!valid(config_) || !source_)
    return;
  if (!source_->open(Format{config_.sample_rate, config_.channels},
                     config_.capacity_frames)) {
    log::w(kTag, "record: the source could not be opened ({} Hz, {} ch)",
           config_.sample_rate, config_.channels);
    return;
  }
  ok_ = true;
}

Recorder::~Recorder() {
  shutdown();
}

void Recorder::start() {
  if (!ok_ || shut_.load(std::memory_order_acquire))
    return;
  source_->start();
  recording_.store(true, std::memory_order_release);
}

void Recorder::stop() {
  if (!ok_)
    return;
  recording_.store(false, std::memory_order_release);
  source_->stop();
}

int Recorder::read(std::int16_t* pcm, int frames, bool blocking) {
  if (!ok_ || shut_.load(std::memory_order_acquire))
    return -1;
  const int n = source_->read(pcm, frames, blocking);
  if (shut_.load(std::memory_order_acquire))
    return -1;
  if (n > 0)
    position_.fetch_add(static_cast<std::uint64_t>(n),
                        std::memory_order_acq_rel);
  return n;
}

void Recorder::shutdown() {
  if (!ok_ || shut_.exchange(true, std::memory_order_acq_rel))
    return;
  recording_.store(false, std::memory_order_release);
  source_->close();
}

}  // namespace acl::audio

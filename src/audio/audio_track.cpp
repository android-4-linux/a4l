// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; Android SDK documentation for
// android.media.AudioTrack. No GPL-3.0 sources.

#include "audio/audio_track.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string_view>

#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio.track";
using Clock = std::chrono::steady_clock;

// How often the pump moves frames to the sink.
constexpr std::chrono::milliseconds kTick{5};
// Frames moved at once, at most: 100 ms. After a stall the pump does not
// send a burst to catch up; it starts the clock again.
constexpr int kMaxCatchUpMs = 100;
// How far ahead of the clock the pump runs. Whatever plays the frames takes
// them a period at a time (about 20 ms for PipeWire), so frames handed over
// exactly as they fall due leave it short at the start of every period. The
// lead is sent at once when playback starts and is the cushion from then on.
constexpr int kLeadMs = 30;
// A sink that reports what it holds unplayed (PipeWire) is kept this full
// and no fuller, so the track follows the device's clock and not its own.
constexpr int kSinkTargetMs = 60;
constexpr std::int64_t kMaxDeviceLatencyNs = 2'000'000'000;
// How long timestamp() waits for a device to say its latency.
constexpr std::chrono::milliseconds kLatencyWait{250};

constexpr int kMinRate = 4000;
constexpr int kMaxRate = 192000;
constexpr int kMaxChannels = 8;
// 30 s at the highest rate: far past any streaming buffer, and room for a
// static sound of some length.
constexpr int kMaxCapacityFrames = 30 * kMaxRate;

std::int64_t monotonic_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

}  // namespace

bool Track::valid(const Config& c) noexcept {
  return c.sample_rate >= kMinRate && c.sample_rate <= kMaxRate &&
         c.channels >= 1 && c.channels <= kMaxChannels &&
         c.capacity_frames >= 1 && c.capacity_frames <= kMaxCapacityFrames;
}

int Track::min_capacity_frames(int sample_rate) noexcept {
  const int rate = std::clamp(sample_rate, kMinRate, kMaxRate);
  return rate / 1000 * 80 + rate % 1000 * 80 / 1000;
}

Track::Track(const Config& config, std::unique_ptr<Sink> sink)
    : config_(config), sink_(std::move(sink)) {
  if (!valid(config_) || !sink_)
    return;
  buf_.resize(static_cast<std::size_t>(config_.capacity_frames) *
              static_cast<std::size_t>(config_.channels));
  if (!sink_->open(Format{config_.sample_rate, config_.channels})) {
    log::w(kTag, "track: the sink could not be opened ({} Hz, {} ch)",
           config_.sample_rate, config_.channels);
    return;
  }
  ok_ = true;
  thread_ = std::thread([this] { pump(); });
}

Track::~Track() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    released_ = true;
    ++epoch_;
  }
  work_.notify_all();
  room_.notify_all();
  if (thread_.joinable())
    thread_.join();
  if (ok_)
    sink_->close();
}

int Track::write(const std::int16_t* pcm, int frames, bool blocking) {
  if (!ok_ || pcm == nullptr || frames <= 0)
    return 0;
  const auto ch = static_cast<std::size_t>(config_.channels);
  const auto cap = static_cast<std::size_t>(config_.capacity_frames);
  std::unique_lock<std::mutex> lock(mu_);
  if (released_)
    return -1;

  if (config_.static_mode) {
    const std::size_t n =
        std::min(static_cast<std::size_t>(frames), cap - fill_);
    std::memcpy(buf_.data() + fill_ * ch, pcm, n * ch * sizeof(std::int16_t));
    fill_ += n;
    return static_cast<int>(n);
  }

  int written = 0;
  const std::uint64_t epoch = epoch_;
  while (written < frames) {
    if (released_)
      return written > 0 ? written : -1;
    const std::size_t space = cap - fill_;
    if (space == 0) {
      // Full. A blocking write waits only while something is taking frames
      // out, and is let go by pause, stop and flush.
      if (!blocking || state_ != State::Playing || epoch_ != epoch)
        break;
      room_.wait(lock);
      continue;
    }
    const std::size_t n =
        std::min(space, static_cast<std::size_t>(frames - written));
    // Two pieces at most: up to the end of the ring, then from its start.
    const std::size_t tail = (head_ + fill_) % cap;
    const std::size_t first = std::min(n, cap - tail);
    std::memcpy(buf_.data() + tail * ch,
                pcm + static_cast<std::size_t>(written) * ch,
                first * ch * sizeof(std::int16_t));
    std::memcpy(buf_.data(),
                pcm + (static_cast<std::size_t>(written) + first) * ch,
                (n - first) * ch * sizeof(std::int16_t));
    fill_ += n;
    written += static_cast<int>(n);
    work_.notify_one();
  }
  return written;
}

void Track::play() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ok_ || released_)
      return;
    if (state_ == State::Stopped && config_.static_mode && draining_ == false &&
        static_pos_ >= fill_)
      static_pos_ = 0;  // a finished sound plays again from its start
    draining_ = false;
    state_ = State::Playing;
    starved_ = false;
  }
  work_.notify_all();
}

void Track::pause() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::Playing)
      return;
    state_ = State::Paused;
    draining_ = false;
    ++epoch_;
  }
  work_.notify_all();
  room_.notify_all();
}

void Track::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++epoch_;
    if (state_ == State::Playing && !config_.static_mode && fill_ > 0) {
      draining_ = true;  // the pump plays the rest out and then stops
    } else {
      state_ = State::Stopped;
      draining_ = false;
      position_ = 0;
      last_ts_frames_ = 0;
      if (config_.static_mode)
        static_pos_ = 0;
    }
  }
  work_.notify_all();
  room_.notify_all();
}

void Track::flush() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == State::Playing && !draining_)
      return;
    if (config_.static_mode)
      return;  // a static sound is not discarded by flush
    head_ = 0;
    fill_ = 0;
    position_ = 0;
    last_ts_frames_ = 0;
    if (draining_) {
      draining_ = false;
      state_ = State::Stopped;
    }
    ++epoch_;
  }
  work_.notify_all();
  room_.notify_all();
}

bool Track::reload_static() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!config_.static_mode || state_ == State::Playing)
    return false;
  static_pos_ = 0;
  position_ = 0;
  last_ts_frames_ = 0;
  return true;
}

void Track::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    released_ = true;
    ++epoch_;
  }
  work_.notify_all();
  room_.notify_all();
}

Track::State Track::state() const {
  std::lock_guard<std::mutex> lock(mu_);
  // A track playing out its last buffer after stop() reports stopped, as the
  // app asked.
  return draining_ ? State::Stopped : state_;
}

std::uint64_t Track::position() const {
  std::uint64_t pos = 0, ahead = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pos = position_;
    // Frames handed to the sink ahead of the clock have not been heard yet.
    if (state_ != State::Paused && since_anchor_ > 0) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                anchor_);
      const std::uint64_t fallen_due =
          static_cast<std::uint64_t>(
              std::max<std::int64_t>(elapsed.count(), 0)) *
          static_cast<std::uint64_t>(config_.sample_rate) / 1'000'000;
      ahead = since_anchor_ > fallen_due ? since_anchor_ - fallen_due : 0;
    }
  }
  // Nor has what the sink says it still holds. The two overlap (the frames
  // ahead of the clock are among those the sink holds), so the larger.
  const std::uint64_t pending = ok_ ? sink_->frames_pending() : 0;
  const std::uint64_t unheard = std::max(ahead, pending);
  return pos > unheard ? pos - unheard : 0;
}

bool Track::timestamp(std::uint64_t* frames, std::int64_t* time_ns) const {
  Clock::time_point first;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!played_any_ || position_ == 0)
      return false;
    first = first_played_;
  }
  // A device that has not yet said how late it is: no timestamp, so that a
  // player does not start from one that is wrong by that much. A device
  // says within a period or two. One that has not by kLatencyWait is not
  // going to (in a --split child, the thread that would hear the broker is
  // busy), and the track goes on without the figure rather than without
  // timestamps.
  std::int64_t latency = ok_ ? sink_->latency_ns() : 0;
  if (latency < 0) {
    if (Clock::now() - first < kLatencyWait)
      return false;
    latency = 0;
  }
  // position() counts a frame as played when the sink has let go of it.
  // The device sounds it later by its own latency, so the frame sounding
  // now is that much earlier in the stream.
  const std::uint64_t pos = position();
  const std::uint64_t behind =
      static_cast<std::uint64_t>(std::min(latency, kMaxDeviceLatencyNs)) *
      static_cast<std::uint64_t>(config_.sample_rate) / 1'000'000'000;
  std::lock_guard<std::mutex> lock(mu_);
  if (position_ == 0)
    return false;  // stopped or flushed meanwhile
  // A timestamp does not go back: a player reads a smaller frame count as
  // the counter having wrapped. When the latency grows (another device) the
  // frame stands still until playback has caught up with it.
  const std::uint64_t now_frames =
      std::max(pos > behind ? pos - behind : 0, last_ts_frames_);
  if (now_frames == 0)
    return false;
  last_ts_frames_ = now_frames;
  *frames = now_frames;
  *time_ns = monotonic_ns();
  return true;
}

std::int64_t Track::device_latency_ns() const {
  // Bounded here as well as in the sinks: this multiplies a sample rate.
  return ok_ ? std::clamp<std::int64_t>(sink_->latency_ns(), 0,
                                        kMaxDeviceLatencyNs)
             : 0;
}

int Track::underruns() const {
  std::lock_guard<std::mutex> lock(mu_);
  return underruns_;
}

int Track::buffered_frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (config_.static_mode)
    return static_cast<int>(fill_ - std::min(static_pos_, fill_));
  return static_cast<int>(fill_);
}

void Track::set_volume(float left, float right) {
  const auto clamp = [](float v) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
  };
  std::lock_guard<std::mutex> lock(mu_);
  gain_l_ = clamp(left);
  gain_r_ = clamp(right);
}

std::uint64_t Track::frames_to_sink() const {
  std::lock_guard<std::mutex> lock(mu_);
  return to_sink_;
}

// Copy up to `max_frames` out of the buffer. Caller holds mu_.
int Track::take_locked(std::int16_t* out, int max_frames) {
  const auto ch = static_cast<std::size_t>(config_.channels);
  if (config_.static_mode) {
    const std::size_t left = fill_ - std::min(static_pos_, fill_);
    const std::size_t n = std::min(left, static_cast<std::size_t>(max_frames));
    std::memcpy(out, buf_.data() + static_pos_ * ch,
                n * ch * sizeof(std::int16_t));
    static_pos_ += n;
    return static_cast<int>(n);
  }
  const auto cap = static_cast<std::size_t>(config_.capacity_frames);
  const std::size_t n = std::min(fill_, static_cast<std::size_t>(max_frames));
  const std::size_t first = std::min(n, cap - head_);
  std::memcpy(out, buf_.data() + head_ * ch, first * ch * sizeof(std::int16_t));
  std::memcpy(out + first * ch, buf_.data(),
              (n - first) * ch * sizeof(std::int16_t));
  head_ = (head_ + n) % cap;
  fill_ -= n;
  return static_cast<int>(n);
}

void Track::apply_gain(std::int16_t* pcm, int frames) const {
  float l = 1.0f, r = 1.0f;
  {
    std::lock_guard<std::mutex> lock(mu_);
    l = gain_l_;
    r = gain_r_;
  }
  if (l == 1.0f && r == 1.0f)
    return;
  const int ch = config_.channels;
  const float mono = (l + r) / 2;
  for (int f = 0; f < frames; ++f) {
    for (int c = 0; c < ch; ++c) {
      const float g = ch == 1 ? mono : (c % 2 == 0 ? l : r);
      std::int16_t& s = pcm[f * ch + c];
      s = static_cast<std::int16_t>(std::lround(static_cast<float>(s) * g));
    }
  }
}

void Track::pump() {
  const int rate = config_.sample_rate;
  const int max_burst = rate / 1000 * (kMaxCatchUpMs + kLeadMs) + 1;
  const std::uint64_t sink_target =
      static_cast<std::uint64_t>(rate) * kSinkTargetMs / 1000;
  const std::uint64_t lead = static_cast<std::uint64_t>(rate) * kLeadMs / 1000;
  std::vector<std::int16_t> out(static_cast<std::size_t>(max_burst) *
                                static_cast<std::size_t>(config_.channels));
  bool running = false;

  std::unique_lock<std::mutex> lock(mu_);
  while (!released_) {
    if (state_ != State::Playing) {
      running = false;
      work_.wait(lock);
      continue;
    }
    if (!running) {
      running = true;
      anchor_ = Clock::now();
      since_anchor_ = 0;
    }
    lock.unlock();
    std::this_thread::sleep_for(kTick);
    // A sink that says what it holds is topped up to its target and no more.
    const std::uint64_t pending = sink_->frames_pending();
    lock.lock();
    if (released_ || state_ != State::Playing) {
      running = false;
      continue;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - anchor_);
    const std::uint64_t owed_total =
        lead + static_cast<std::uint64_t>(elapsed.count()) *
                   static_cast<std::uint64_t>(rate) / 1'000'000;
    std::uint64_t due =
        owed_total > since_anchor_ ? owed_total - since_anchor_ : 0;
    if (due > static_cast<std::uint64_t>(max_burst)) {
      // Stalled (the process was stopped, the machine slept). Start the
      // clock again and do not send the backlog as a burst.
      anchor_ = Clock::now();
      since_anchor_ = 0;
      due = 0;
    }
    if (pending >= sink_target) {
      // The device is behind this clock. Wait for it; do not count the wait
      // as time owed.
      anchor_ = Clock::now();
      since_anchor_ = 0;
      due = 0;
    }
    if (due == 0)
      continue;

    const int got = take_locked(out.data(), static_cast<int>(due));
    const bool empty = config_.static_mode ? static_pos_ >= fill_ : fill_ == 0;
    if (got < static_cast<int>(due)) {
      // Ran out. The clock restarts when data comes, so that what was not
      // there is not owed later. Until then the anchor stands, so that the
      // last frames handed over are still counted as they fall due.
      since_anchor_ += static_cast<std::uint64_t>(got);
      running = false;
      if (draining_ || (config_.static_mode && empty)) {
        // The end of what stop() asked to be played out, or of the sound.
        state_ = State::Stopped;
        draining_ = false;
        if (!config_.static_mode) {
          position_ = 0;
          last_ts_frames_ = 0;
        }
      } else if (!starved_ && played_any_) {
        starved_ = true;
        ++underruns_;
      }
    } else {
      since_anchor_ += static_cast<std::uint64_t>(got);
      starved_ = false;
    }
    if (got > 0) {
      const bool stopped_now = state_ == State::Stopped;
      if (!stopped_now || config_.static_mode)
        position_ += static_cast<std::uint64_t>(got);
      to_sink_ += static_cast<std::uint64_t>(got);
      if (!played_any_)
        first_played_ = Clock::now();
      played_any_ = true;
      lock.unlock();
      room_.notify_all();
      apply_gain(out.data(), got);
      sink_->write(out.data(), got * config_.channels);
      lock.lock();
    }
    if (state_ != State::Playing) {
      room_.notify_all();
      continue;
    }
    if (empty && !config_.static_mode && !draining_) {
      // Nothing to play: sleep until a writer brings some, instead of
      // waking every tick.
      running = false;
      work_.wait(lock, [this] {
        return released_ || state_ != State::Playing || fill_ > 0 || draining_;
      });
    }
  }
}

}  // namespace acl::audio

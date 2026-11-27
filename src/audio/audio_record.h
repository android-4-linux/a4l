// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; Android SDK documentation for
// android.media.AudioRecord (states, blocking and non-blocking reads, buffer
// overrun). No GPL-3.0 sources.
//
// The engine under android.media.AudioRecord: 16-bit PCM read from an
// audio::Source at the stream's own rate.
//
// A Source is where the samples come from. The only one so far is silence,
// produced by the clock: a4l opens no microphone yet. An app that records
// gets a stream that starts, paces a blocking read as a device would, and
// stops, with nothing in it.
#ifndef ACL_AUDIO_AUDIO_RECORD_H
#define ACL_AUDIO_AUDIO_RECORD_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include "audio/audio.h"

namespace acl::audio {

// Interleaved signed-16-bit PCM in, as Sink is PCM out.
class Source {
 public:
  virtual ~Source() = default;
  // `capacity_frames` is how much the source keeps for a reader that is
  // late. Past that the oldest frames are dropped.
  virtual bool open(const Format& fmt, int capacity_frames) = 0;
  virtual void start() = 0;
  // A blocked read returns with what it has.
  virtual void stop() = 0;
  // Up to `frames` frames into `pcm`. Blocking: waits for all of them while
  // the source is started. Not blocking: what is there now. Returns frames.
  virtual int read(std::int16_t* pcm, int frames, bool blocking) = 0;
  virtual void close() = 0;
};

// The configured source. Silence: see the note at the top.
std::unique_ptr<Source> make_source();

// Silence at the format's rate by the monotonic clock.
std::unique_ptr<Source> make_silent_source();

class Recorder {
 public:
  struct Config {
    int sample_rate = 48000;
    int channels = 1;
    // How much is kept for a reader that is late.
    int capacity_frames = 1920;
  };

  // Bounds a Config must meet; what native_setup checks before making one.
  static bool valid(const Config& c) noexcept;
  // The smallest buffer worth having at `sample_rate`: 40 ms. 0 for a rate
  // out of bounds.
  static int min_capacity_frames(int sample_rate) noexcept;

  // `source` is opened here. ok() is false if it could not be.
  Recorder(const Config& config, std::unique_ptr<Source> source);
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  bool ok() const noexcept { return ok_; }
  const Config& config() const noexcept { return config_; }

  void start();
  void stop();
  bool recording() const noexcept {
    return recording_.load(std::memory_order_acquire);
  }

  // Read up to `frames` frames. Returns how many, 0 when stopped and nothing
  // is left, or -1 if the recorder is shut down.
  int read(std::int16_t* pcm, int frames, bool blocking);

  // Frames read since the recorder was made.
  std::uint64_t position() const noexcept {
    return position_.load(std::memory_order_acquire);
  }

  // End the recorder without destroying it: every blocked or later read
  // returns. For an owner that cannot destroy it yet because a read still
  // holds it.
  void shutdown();

 private:
  const Config config_;
  std::unique_ptr<Source> source_;
  bool ok_ = false;
  std::atomic<bool> recording_{false};
  std::atomic<bool> shut_{false};
  std::atomic<std::uint64_t> position_{0};
};

}  // namespace acl::audio

#endif  // ACL_AUDIO_AUDIO_RECORD_H

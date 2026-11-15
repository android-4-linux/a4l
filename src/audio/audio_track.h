// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; Android SDK documentation for
// android.media.AudioTrack (states, blocking and non-blocking writes, playback
// head position, static mode). No GPL-3.0 sources.
//
// The engine under android.media.AudioTrack: a bounded buffer of 16-bit PCM
// that a pump thread drains into an audio::Sink at the stream's own rate.
//
// A Sink takes whatever it is given at once. AudioTrack does not: a blocking
// write returns when the data has been taken, and the data is taken as fast as
// it plays, which is how an app that writes in a loop, or a media player's
// audio renderer, is paced. So the pacing is here. The buffer holds
// `capacity_frames`; the pump moves frames to the sink by the clock; a writer
// waits for room. Nothing depends on the device reporting progress, so the
// same engine works where the sink is in another process (--split).
#ifndef ACL_AUDIO_AUDIO_TRACK_H
#define ACL_AUDIO_AUDIO_TRACK_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/audio.h"

namespace acl::audio {

class Track {
 public:
  struct Config {
    int sample_rate = 48000;
    int channels = 2;
    // The buffer. Streaming: how much may be written ahead of playback.
    // Static: the whole sound.
    int capacity_frames = 4096;
    bool static_mode = false;
  };
  enum class State { Stopped, Paused, Playing };

  // Bounds a Config must meet; what native_setup checks before making one.
  static bool valid(const Config& c) noexcept;
  // The smallest buffer worth having at `sample_rate`: 80 ms.
  static int min_capacity_frames(int sample_rate) noexcept;

  // `sink` is opened here. ok() is false if it could not be.
  Track(const Config& config, std::unique_ptr<Sink> sink);
  ~Track();
  Track(const Track&) = delete;
  Track& operator=(const Track&) = delete;

  bool ok() const noexcept { return ok_; }
  const Config& config() const noexcept { return config_; }

  // Write `frames` frames of interleaved samples. Returns how many were
  // taken, or -1 if the track is released.
  //  - Streaming, blocking: waits for room while the track is playing.
  //    Returns early, with what it took, if the track is paused, stopped,
  //    flushed or released meanwhile: a write must not outlive the playback
  //    it was waiting on.
  //  - Streaming, not blocking: takes what fits now.
  //  - Static: copies into the sound from where the last write ended.
  int write(const std::int16_t* pcm, int frames, bool blocking);

  void play();
  // Stop taking from the buffer. What is in it stays.
  void pause();
  // Streaming: what is in the buffer is played out, then the track is
  // stopped and its position goes back to 0. Static: stops at once.
  void stop();
  // Discard what is buffered and reset the position. Ignored while playing,
  // as on Android.
  void flush();
  // Static: play the sound from the start next time. False while playing.
  bool reload_static();

  State state() const;
  // Frames played since the position was last reset.
  std::uint64_t position() const;
  // The position together with the time it was read at (CLOCK_MONOTONIC).
  // False until something has played.
  bool timestamp(std::uint64_t* frames, std::int64_t* time_ns) const;
  // Times playback ran out of data while playing.
  int underruns() const;
  // Frames buffered and not yet played.
  int buffered_frames() const;

  // End the track without destroying it: playback stops and every blocked
  // or later write returns. For an owner that cannot destroy the track yet
  // because a write still holds it.
  void shutdown();

  // Gain per side, 0 to 1. Mono takes the mean.
  void set_volume(float left, float right);

  // Total frames the sink has been given, for tests.
  std::uint64_t frames_to_sink() const;

 private:
  void pump();
  int take_locked(std::int16_t* out, int max_frames);
  void apply_gain(std::int16_t* pcm, int frames) const;

  const Config config_;
  std::unique_ptr<Sink> sink_;
  bool ok_ = false;

  mutable std::mutex mu_;
  std::condition_variable room_;  // a writer waits for room, or to be let go
  std::condition_variable work_;  // the pump waits for something to do
  // Streaming: a ring of capacity_frames. Static: the sound, played from
  // static_pos_.
  std::vector<std::int16_t> buf_;
  std::size_t head_ = 0;        // ring: index of the oldest frame
  std::size_t fill_ = 0;        // ring: frames held; static: frames written
  std::size_t static_pos_ = 0;  // static: next frame to play
  State state_ = State::Stopped;
  bool draining_ = false;  // stop() was called; play out, then stop
  bool released_ = false;
  std::uint64_t epoch_ = 0;  // bumped by pause/stop/flush: lets a writer go
  std::uint64_t position_ = 0;
  std::uint64_t to_sink_ = 0;
  bool starved_ = false;  // in an underrun now
  int underruns_ = 0;
  bool played_any_ = false;
  float gain_l_ = 1.0f, gain_r_ = 1.0f;
  // The clock the stream plays by: `since_anchor_` frames have gone to the
  // sink since `anchor_`. The pump runs a little ahead of it (the lead), and
  // position() counts a frame as played when the clock reaches it, not when
  // it was handed over.
  std::chrono::steady_clock::time_point anchor_{};
  std::uint64_t since_anchor_ = 0;

  std::thread thread_;
};

}  // namespace acl::audio

#endif  // ACL_AUDIO_AUDIO_TRACK_H

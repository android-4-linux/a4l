// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; Android SDK documentation for
// android.media.ToneGenerator (each tone's frequencies and cadence as its
// constant documents them). No GPL-3.0 sources.
//
// The engine under android.media.ToneGenerator: ToneSynth renders a tone
// type to 16-bit mono PCM, TonePlayer plays it through an audio::Track.
//
// The DTMF, call supervisory and proprietary tones (types 0 to 44) sound.
// Where the documentation gives a tone per region the CEPT one is used. The
// CDMA alert and ISDN signal patterns (types 45 and up) are accepted and
// play nothing.
#ifndef ACL_AUDIO_TONE_GENERATOR_H
#define ACL_AUDIO_TONE_GENERATOR_H

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "audio/audio.h"
#include "audio/audio_track.h"

namespace acl::audio {

// One step of a tone: up to three frequencies sounding together, or silence
// when all are 0. ms 0 lasts until the tone is stopped.
struct ToneSegment {
  std::uint16_t ms;
  std::array<std::uint16_t, 3> hz;
};

class ToneSynth {
 public:
  // The highest tone type android.media.ToneGenerator defines.
  static constexpr int kMaxTone = 98;
  static bool known(int tone) noexcept { return tone >= 0 && tone <= kMaxTone; }

  // `limit_ms` below 0 plays the tone's own length, which for many is until
  // stopped. Otherwise the tone ends at whichever comes first.
  ToneSynth(int tone, int sample_rate, int limit_ms);

  // Render up to `frames` mono frames. Returns how many; fewer than asked
  // means the tone has ended.
  int render(std::int16_t* out, int frames);
  bool done() const noexcept { return done_; }

 private:
  void enter_segment();

  const ToneSegment* segments_ = nullptr;
  int n_segments_ = 0;
  int cycles_left_ = 0;  // 0: repeats until stopped
  bool forever_ = false;
  int rate_;
  std::int64_t limit_frames_;  // -1: none
  std::int64_t played_ = 0;
  int index_ = 0;
  std::int64_t seg_frames_ = 0;  // -1: until stopped
  std::int64_t seg_pos_ = 0;
  std::array<double, 3> step_{};  // radians per frame
  int ramp_ = 1;
  bool done_ = false;
};

class TonePlayer {
 public:
  using SinkFactory = std::function<std::unique_ptr<Sink>()>;

  static constexpr int kSampleRate = 48000;

  // `volume` is a percentage, 0 to 100. Nothing is opened until a tone is
  // started.
  explicit TonePlayer(int volume, SinkFactory make_sink = nullptr);
  ~TonePlayer();
  TonePlayer(const TonePlayer&) = delete;
  TonePlayer& operator=(const TonePlayer&) = delete;

  // Play `tone`, replacing one that is playing. False for a type that does
  // not exist or if the audio output cannot be opened.
  bool start(int tone, int duration_ms);
  void stop();

 private:
  void run();

  const float gain_;
  const SinkFactory make_sink_;
  std::mutex mu_;
  std::condition_variable wake_;
  std::unique_ptr<Track> track_;
  std::uint64_t generation_ = 0;  // bumped by start and stop
  bool pending_ = false;
  bool quit_ = false;
  int tone_ = 0;
  int limit_ms_ = -1;
  std::thread thread_;
};

}  // namespace acl::audio

#endif  // ACL_AUDIO_TONE_GENERATOR_H

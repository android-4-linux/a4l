// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; Android SDK documentation for
// android.media.ToneGenerator (each tone's frequencies and cadence as its
// constant documents them). No GPL-3.0 sources.
#include "audio/tone_generator.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

namespace acl::audio {
namespace {

constexpr double kTwoPi = 6.283185307179586;
// Each frequency's share of full scale. Three together stay below it.
constexpr double kPartAmplitude = 0.25 * 32767.0;
// Fade at each end of a burst, so it does not click.
constexpr int kRampMs = 4;
constexpr int kChunkFrames = TonePlayer::kSampleRate / 100;  // 10 ms

struct ToneDef {
  const ToneSegment* segments;
  int count;
  int cycles;  // 0: repeat until stopped
};

#define ACL_TONE(name, cycles, ...)                   \
  constexpr ToneSegment name##_seg[] = {__VA_ARGS__}; \
  constexpr ToneDef name = {name##_seg, std::size(name##_seg), cycles}

// DTMF, by tone type: keys 0 to 9, *, #, A to D. Column frequency, then row.
constexpr ToneSegment kDtmfSeg[16] = {
    {0, {1336, 941, 0}}, {0, {1209, 697, 0}}, {0, {1336, 697, 0}},
    {0, {1477, 697, 0}}, {0, {1209, 770, 0}}, {0, {1336, 770, 0}},
    {0, {1477, 770, 0}}, {0, {1209, 852, 0}}, {0, {1336, 852, 0}},
    {0, {1477, 852, 0}}, {0, {1209, 941, 0}}, {0, {1477, 941, 0}},
    {0, {1633, 697, 0}}, {0, {1633, 770, 0}}, {0, {1633, 852, 0}},
    {0, {1633, 941, 0}},
};

// Call supervisory tones, CEPT.
ACL_TONE(kDial, 1, {0, {425, 0, 0}});
ACL_TONE(kBusy, 0, {500, {425, 0, 0}}, {500, {0, 0, 0}});
ACL_TONE(kCongestion, 0, {200, {425, 0, 0}}, {200, {0, 0, 0}});
ACL_TONE(kRadioAck, 1, {200, {425, 0, 0}});
ACL_TONE(kRadioNotAvail, 3, {200, {425, 0, 0}}, {200, {0, 0, 0}});
ACL_TONE(kError, 0, {330, {950, 1400, 1800}}, {1000, {0, 0, 0}});
ACL_TONE(kCallWaiting,
         0,
         {200, {425, 0, 0}},
         {600, {0, 0, 0}},
         {200, {425, 0, 0}},
         {3000, {0, 0, 0}});
ACL_TONE(kRingtone, 0, {1000, {425, 0, 0}}, {4000, {0, 0, 0}});
// Proprietary tones.
ACL_TONE(kBeep, 1, {35, {400, 1200, 0}});
ACL_TONE(kAck, 2, {100, {1200, 0, 0}}, {100, {0, 0, 0}});
ACL_TONE(kNack, 1, {400, {300, 400, 500}});
ACL_TONE(kPrompt, 1, {200, {400, 1200, 0}});
ACL_TONE(kBeep2,
         1,
         {35, {400, 1200, 0}},
         {200, {0, 0, 0}},
         {35, {400, 1200, 0}});
// IS-95 supervisory tones and the CDMA network tones.
ACL_TONE(kIntercept, 0, {250, {440, 0, 0}}, {250, {620, 0, 0}});
ACL_TONE(kInterceptAbbrev, 8, {250, {440, 0, 0}}, {250, {620, 0, 0}});
ACL_TONE(kInterceptOnce, 1, {250, {440, 0, 0}}, {250, {620, 0, 0}});
ACL_TONE(kReorder, 0, {250, {480, 620, 0}}, {250, {0, 0, 0}});
ACL_TONE(kReorderAbbrev, 8, {250, {480, 620, 0}}, {250, {0, 0, 0}});
ACL_TONE(kConfirm, 3, {100, {350, 440, 0}}, {100, {0, 0, 0}});
ACL_TONE(kPip, 4, {100, {480, 0, 0}}, {100, {0, 0, 0}});
ACL_TONE(kUsaRingback, 0, {2000, {440, 480, 0}}, {4000, {0, 0, 0}});
ACL_TONE(kNetworkBusy, 0, {500, {480, 620, 0}}, {500, {0, 0, 0}});
ACL_TONE(kNetworkCallWaiting, 1, {300, {440, 0, 0}});

#undef ACL_TONE

constexpr ToneDef kSilent = {nullptr, 0, 1};

ToneDef definition(int tone) {
  if (tone >= 0 && tone < 16)
    return {&kDtmfSeg[tone], 1, 1};
  switch (tone) {
    case 16:
      return kDial;
    case 17:
      return kBusy;
    case 18:
      return kCongestion;
    case 19:
      return kRadioAck;
    case 20:
      return kRadioNotAvail;
    case 21:
      return kError;
    case 22:
      return kCallWaiting;
    case 23:
      return kRingtone;
    case 24:
      return kBeep;
    case 25:
      return kAck;
    case 26:
      return kNack;
    case 27:
      return kPrompt;
    case 28:
      return kBeep2;
    case 29:
    case 36:
      return kIntercept;
    case 30:
      return kInterceptAbbrev;
    case 31:
    case 39:
      return kReorderAbbrev;
    case 32:
    case 41:
      return kConfirm;
    case 33:
    case 44:
      return kPip;
    case 34:
      return kDial;
    case 35:
      return kUsaRingback;
    case 37:
      return kInterceptOnce;
    case 38:
      return kReorder;
    case 40:
      return kNetworkBusy;
    case 43:
      return kNetworkCallWaiting;
    default:
      return kSilent;
  }
}

}  // namespace

ToneSynth::ToneSynth(int tone, int sample_rate, int limit_ms)
    : rate_(std::max(sample_rate, 1000)),
      limit_frames_(limit_ms < 0
                        ? -1
                        : static_cast<std::int64_t>(limit_ms) * rate_ / 1000) {
  const ToneDef def = known(tone) ? definition(tone) : kSilent;
  segments_ = def.segments;
  n_segments_ = def.count;
  forever_ = def.cycles == 0;
  cycles_left_ = def.cycles;
  ramp_ = std::max(1, rate_ * kRampMs / 1000);
  if (n_segments_ == 0 || limit_frames_ == 0)
    done_ = true;
  else
    enter_segment();
}

void ToneSynth::enter_segment() {
  const ToneSegment& seg = segments_[index_];
  seg_frames_ =
      seg.ms == 0 ? -1 : static_cast<std::int64_t>(seg.ms) * rate_ / 1000;
  seg_pos_ = 0;
  for (std::size_t i = 0; i < step_.size(); ++i)
    step_[i] = kTwoPi * seg.hz[i] / rate_;
}

int ToneSynth::render(std::int16_t* out, int frames) {
  int n = 0;
  while (n < frames && !done_) {
    if (limit_frames_ >= 0 && played_ >= limit_frames_) {
      done_ = true;
      break;
    }
    if (seg_frames_ >= 0 && seg_pos_ >= seg_frames_) {
      if (++index_ == n_segments_) {
        index_ = 0;
        if (!forever_ && --cycles_left_ == 0) {
          done_ = true;
          break;
        }
      }
      enter_segment();
      continue;
    }
    // Frames from here to the nearer end: of this burst, or of the tone.
    std::int64_t to_end = seg_frames_ < 0 ? ramp_ : seg_frames_ - seg_pos_;
    if (limit_frames_ >= 0)
      to_end = std::min(to_end, limit_frames_ - played_);
    const double gain =
        std::min({1.0, static_cast<double>(seg_pos_ + 1) / ramp_,
                  static_cast<double>(to_end) / ramp_});
    double sample = 0.0;
    for (const double step : step_) {
      if (step > 0.0)
        sample += std::sin(step * static_cast<double>(seg_pos_));
    }
    out[n++] =
        static_cast<std::int16_t>(std::lround(sample * gain * kPartAmplitude));
    ++seg_pos_;
    ++played_;
  }
  return n;
}

TonePlayer::TonePlayer(int volume, SinkFactory make_sink)
    : gain_(static_cast<float>(std::clamp(volume, 0, 100)) / 100.0f),
      make_sink_(std::move(make_sink)) {}

TonePlayer::~TonePlayer() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    quit_ = true;
    ++generation_;
    if (track_)
      track_->shutdown();
  }
  wake_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

bool TonePlayer::start(int tone, int duration_ms) {
  if (!ToneSynth::known(tone))
    return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!track_) {
      Track::Config config;
      config.sample_rate = kSampleRate;
      config.channels = 1;
      config.capacity_frames = Track::min_capacity_frames(kSampleRate);
      auto track = std::make_unique<Track>(
          config, make_sink_ ? make_sink_() : make_sink());
      if (!track->ok())
        return false;
      track->set_volume(gain_, gain_);
      track_ = std::move(track);
      thread_ = std::thread([this] { run(); });
    }
    ++generation_;
    tone_ = tone;
    limit_ms_ = duration_ms;
    pending_ = true;
    // Lets go of a write the tone being replaced is blocked in.
    track_->pause();
  }
  wake_.notify_all();
  return true;
}

void TonePlayer::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  ++generation_;
  pending_ = false;
  if (track_) {
    track_->pause();
    track_->flush();
  }
}

void TonePlayer::run() {
  std::vector<std::int16_t> chunk(kChunkFrames);
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    wake_.wait(lock, [this] { return pending_ || quit_; });
    if (quit_)
      return;
    pending_ = false;
    const std::uint64_t mine = generation_;
    ToneSynth synth(tone_, kSampleRate, limit_ms_);
    // Drops what a replaced tone left behind.
    track_->pause();
    track_->flush();
    track_->play();
    while (generation_ == mine) {
      lock.unlock();
      const int n = synth.render(chunk.data(), kChunkFrames);
      const int taken = n > 0 ? track_->write(chunk.data(), n, true) : 0;
      lock.lock();
      if (generation_ != mine)
        break;
      if (n < kChunkFrames || taken < n) {
        track_->stop();  // plays out what is buffered
        break;
      }
    }
  }
}

}  // namespace acl::audio

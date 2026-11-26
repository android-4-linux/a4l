// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test; Android SDK documentation for
// android.media.ToneGenerator (the tones asserted). No GPL-3.0 sources.
//
// audio::ToneSynth and audio::TonePlayer, the engine under
// android.media.ToneGenerator: a tone has the frequencies and the length its
// type documents, and the player starts, replaces and stops one.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "audio/tone_generator.h"

using namespace acl;
using audio::TonePlayer;
using audio::ToneSynth;

namespace {

constexpr int kRate = 48000;
// android.media.ToneGenerator's constants.
constexpr int kDtmf1 = 1;
constexpr int kSupBusy = 17;
constexpr int kPropBeep = 24;
constexpr int kPropAck = 25;
constexpr int kCdmaAnswer = 42;
constexpr int kCdmaHighL = 53;

// Render until the tone ends or `max_frames` have come out.
std::vector<std::int16_t> render(int tone, int limit_ms, int max_frames) {
  ToneSynth synth(tone, kRate, limit_ms);
  std::vector<std::int16_t> out(static_cast<std::size_t>(max_frames));
  int n = 0;
  while (n < max_frames) {
    const int want = std::min(480, max_frames - n);
    const int got = synth.render(out.data() + n, want);
    n += got;
    if (got < want)
      break;
  }
  out.resize(static_cast<std::size_t>(n));
  return out;
}

// How much of `hz` is in `pcm`, as an amplitude in sample units (Goertzel).
double level(const std::vector<std::int16_t>& pcm, double hz) {
  const double w = 2.0 * 3.141592653589793 * hz / kRate;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (const std::int16_t v : pcm) {
    const double s0 = v + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  return 2.0 * std::sqrt(std::max(power, 0.0)) /
         static_cast<double>(pcm.size());
}

int peak(const std::vector<std::int16_t>& pcm,
         std::size_t from,
         std::size_t to) {
  int p = 0;
  for (std::size_t i = from; i < to && i < pcm.size(); ++i)
    p = std::max(p, std::abs(static_cast<int>(pcm[i])));
  return p;
}

struct Heard {
  std::mutex mu;
  std::vector<std::int16_t> samples;
  int channels = 0;
  int rate = 0;
};

class RecordingSink final : public audio::Sink {
 public:
  explicit RecordingSink(Heard& heard) : heard_(heard) {}
  bool open(const audio::Format& fmt) override {
    std::lock_guard<std::mutex> lock(heard_.mu);
    heard_.channels = fmt.channels;
    heard_.rate = fmt.sample_rate;
    return true;
  }
  int write(const std::int16_t* pcm, int n) override {
    std::lock_guard<std::mutex> lock(heard_.mu);
    heard_.samples.insert(heard_.samples.end(), pcm, pcm + n);
    return n;
  }
  void drain() override {}
  void close() override {}
  std::uint64_t frames_written() const override { return 0; }

 private:
  Heard& heard_;
};

std::size_t heard_frames(Heard& h) {
  std::lock_guard<std::mutex> lock(h.mu);
  return h.samples.size();
}

template <typename F>
bool eventually(int ms, F done) {
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

TonePlayer::SinkFactory into(Heard& heard) {
  return [&heard] { return std::make_unique<RecordingSink>(heard); };
}

}  // namespace

TEST_CASE("ToneSynth: a DTMF key is its two frequencies", "[audio][tone]") {
  const auto pcm = render(kDtmf1, 100, kRate);
  REQUIRE(pcm.size() == 4800);  // the limit ends a continuous tone
  CHECK(level(pcm, 1209) > 6000);
  CHECK(level(pcm, 697) > 6000);
  CHECK(level(pcm, 941) < 600);
  CHECK(level(pcm, 1477) < 600);
  // Fades in and out.
  CHECK(std::abs(pcm.front()) < 200);
  CHECK(std::abs(pcm.back()) < 2000);
}

TEST_CASE("ToneSynth: a tone of bursts has its documented length",
          "[audio][tone]") {
  // 1200 Hz, 100 ms on and 100 ms off, twice.
  const auto pcm = render(kPropAck, -1, kRate);
  REQUIRE(pcm.size() == 19200);
  CHECK(peak(pcm, 1000, 4000) > 6000);
  CHECK(peak(pcm, 4800, 9600) == 0);
  CHECK(peak(pcm, 10600, 13600) > 6000);
  CHECK(peak(pcm, 14400, 19200) == 0);
  // A shorter limit cuts it.
  CHECK(render(kPropAck, 50, kRate).size() == 2400);
  // A longer one does not extend it.
  CHECK(render(kPropAck, 5000, kRate).size() == 19200);
}

TEST_CASE("ToneSynth: a repeating tone does not end", "[audio][tone]") {
  // 500 ms on, 500 ms off, until stopped.
  const auto pcm = render(kSupBusy, -1, 3 * kRate);
  REQUIRE(pcm.size() == static_cast<std::size_t>(3 * kRate));
  CHECK(peak(pcm, 2 * kRate + 1000, 2 * kRate + 20000) > 6000);
  CHECK(peak(pcm, 2 * kRate + 24000, 3 * kRate) == 0);
}

TEST_CASE("ToneSynth: silent and unknown types play nothing", "[audio][tone]") {
  CHECK(render(kCdmaAnswer, -1, kRate).empty());
  CHECK(render(kCdmaHighL, -1, kRate).empty());
  CHECK(render(-1, -1, kRate).empty());
  CHECK(render(500, -1, kRate).empty());
  CHECK(render(kDtmf1, 0, kRate).empty());
  CHECK(ToneSynth::known(0));
  CHECK(ToneSynth::known(98));
  CHECK_FALSE(ToneSynth::known(-1));
  CHECK_FALSE(ToneSynth::known(99));
}

TEST_CASE("TonePlayer: a tone reaches the sink whole", "[audio][tone]") {
  Heard heard;
  {
    TonePlayer player(100, into(heard));
    CHECK_FALSE(player.start(-1, -1));
    CHECK_FALSE(player.start(99, -1));
    CHECK(heard_frames(heard) == 0);       // nothing opened for a refused tone
    REQUIRE(player.start(kPropBeep, -1));  // 35 ms
    REQUIRE(eventually(3000, [&] { return heard_frames(heard) >= 1680; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(heard_frames(heard) == 1680);
    CHECK(heard.channels == 1);
    CHECK(heard.rate == TonePlayer::kSampleRate);
    // It plays again on the same player.
    REQUIRE(player.start(kPropBeep, -1));
    REQUIRE(eventually(3000, [&] { return heard_frames(heard) >= 3360; }));
  }
  CHECK(heard_frames(heard) == 3360);
}

TEST_CASE("TonePlayer: stop ends a continuous tone", "[audio][tone]") {
  Heard heard;
  TonePlayer player(50, into(heard));
  REQUIRE(player.start(kDtmf1, -1));
  REQUIRE(eventually(3000, [&] { return heard_frames(heard) >= 4800; }));
  player.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const std::size_t at_stop = heard_frames(heard);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(heard_frames(heard) == at_stop);
  // Half volume is half the level.
  std::vector<std::int16_t> pcm;
  {
    std::lock_guard<std::mutex> lock(heard.mu);
    pcm.assign(heard.samples.begin(), heard.samples.begin() + 4800);
  }
  const double l = level(pcm, 1209);
  CHECK(l > 3000);
  CHECK(l < 5000);
}

TEST_CASE("TonePlayer: a new tone replaces the one playing", "[audio][tone]") {
  Heard heard;
  TonePlayer player(100, into(heard));
  REQUIRE(player.start(kDtmf1, -1));
  REQUIRE(eventually(3000, [&] { return heard_frames(heard) >= 2400; }));
  REQUIRE(player.start(kPropBeep, -1));
  // The beep ends by itself, so the sink stops being fed.
  std::size_t last = 0;
  REQUIRE(eventually(3000, [&] {
    const std::size_t now = heard_frames(heard);
    const bool still = now == last;
    last = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    return still && now == heard_frames(heard);
  }));
}

TEST_CASE("TonePlayer: destroyed while playing", "[audio][tone]") {
  Heard heard;
  auto player = std::make_unique<TonePlayer>(100, into(heard));
  REQUIRE(player->start(kSupBusy, -1));
  REQUIRE(eventually(3000, [&] { return heard_frames(heard) > 0; }));
  player.reset();  // returns
  SUCCEED();
}

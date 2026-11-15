// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test; Android SDK documentation for
// android.media.AudioTrack (the behavior asserted). No GPL-3.0 sources.
//
// audio::Track, the engine under android.media.AudioTrack: every sample
// written reaches the sink once and in order, a blocking write takes as long
// as the audio it writes, and pause, stop, flush and release each let a
// blocked writer go.
//
// The timing cases use an 8 kHz stream and bounds that are loose on the slow
// side, since the pump is a thread on a shared machine: what they establish
// is that writing does not return at once, not how exactly it is paced.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "audio/audio_track.h"

using namespace acl;
using audio::Track;
using Clock = std::chrono::steady_clock;

namespace {

struct Heard {
  std::mutex mu;
  std::vector<std::int16_t> samples;
  int opens = 0, closes = 0;
  int bad_writes = 0;  // not a whole number of frames
  audio::Format format;
  bool open_fails = false;
};

class RecordingSink final : public audio::Sink {
 public:
  explicit RecordingSink(Heard& heard) : heard_(heard) {}
  bool open(const audio::Format& fmt) override {
    std::lock_guard<std::mutex> lock(heard_.mu);
    if (heard_.open_fails)
      return false;
    ++heard_.opens;
    heard_.format = fmt;
    return true;
  }
  int write(const std::int16_t* pcm, int n) override {
    std::lock_guard<std::mutex> lock(heard_.mu);
    if (n <= 0 || n % heard_.format.channels != 0)
      ++heard_.bad_writes;
    heard_.samples.insert(heard_.samples.end(), pcm, pcm + n);
    return n;
  }
  void drain() override {}
  void close() override {
    std::lock_guard<std::mutex> lock(heard_.mu);
    ++heard_.closes;
  }
  std::uint64_t frames_written() const override { return 0; }

 private:
  Heard& heard_;
};

Track::Config config(int rate,
                     int channels,
                     int capacity,
                     bool is_static = false) {
  Track::Config c;
  c.sample_rate = rate;
  c.channels = channels;
  c.capacity_frames = capacity;
  c.static_mode = is_static;
  return c;
}

std::unique_ptr<Track> make(Heard& heard, const Track::Config& c) {
  return std::make_unique<Track>(c, std::make_unique<RecordingSink>(heard));
}

// A ramp that never repeats within a test, so order and loss both show.
std::vector<std::int16_t> ramp(int samples, int start = 1) {
  std::vector<std::int16_t> v(static_cast<std::size_t>(samples));
  for (int i = 0; i < samples; ++i)
    v[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(start + i);
  return v;
}

std::vector<std::int16_t> heard_copy(Heard& h) {
  std::lock_guard<std::mutex> lock(h.mu);
  return h.samples;
}

// Wait up to `ms` for `done`.
template <typename F>
bool eventually(int ms, F done) {
  const auto until = Clock::now() + std::chrono::milliseconds(ms);
  while (Clock::now() < until) {
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

}  // namespace

TEST_CASE("track: a config is bounded", "[audio][track]") {
  CHECK(Track::valid(config(48000, 2, 4096)));
  CHECK(Track::valid(config(4000, 1, 1)));
  CHECK(Track::valid(config(192000, 8, 192000)));
  CHECK_FALSE(Track::valid(config(0, 2, 4096)));
  CHECK_FALSE(Track::valid(config(3999, 2, 4096)));
  CHECK_FALSE(Track::valid(config(192001, 2, 4096)));
  CHECK_FALSE(Track::valid(config(48000, 0, 4096)));
  CHECK_FALSE(Track::valid(config(48000, 9, 4096)));
  CHECK_FALSE(Track::valid(config(48000, 2, 0)));
  CHECK_FALSE(Track::valid(config(48000, 2, -1)));
  CHECK_FALSE(Track::valid(config(48000, 2, 0x7fffffff)));
  // 80 ms.
  CHECK(Track::min_capacity_frames(48000) == 3840);
  CHECK(Track::min_capacity_frames(44100) == 3528);
  CHECK(Track::min_capacity_frames(8000) == 640);
  CHECK(Track::min_capacity_frames(-5) == Track::min_capacity_frames(4000));

  Heard heard;
  CHECK_FALSE(make(heard, config(0, 2, 4096))->ok());
  heard.open_fails = true;
  CHECK_FALSE(make(heard, config(48000, 2, 4096))->ok());
  CHECK(heard.closes == 0);  // a sink that never opened is not closed
}

TEST_CASE("track: what is written is what the sink hears, once and in order",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 2, 400));
  REQUIRE(t->ok());
  CHECK(heard.format.sample_rate == 8000);
  CHECK(heard.format.channels == 2);
  // 4000 frames, half a second, through a 400-frame buffer: the ring wraps
  // ten times.
  const std::vector<std::int16_t> pcm = ramp(8000);
  t->play();
  int written = 0;
  while (written < 4000) {
    const int n = t->write(pcm.data() + written * 2,
                           4000 - written < 173 ? 4000 - written : 173, true);
    REQUIRE(n > 0);
    written += n;
  }
  REQUIRE(eventually(2000, [&] { return heard_copy(heard).size() == 8000; }));
  CHECK(heard_copy(heard) == pcm);
  CHECK(heard.bad_writes == 0);
  CHECK(t->frames_to_sink() == 4000);
  CHECK(eventually(1000, [&] { return t->position() == 4000; }));
  CHECK(t->buffered_frames() == 0);
  t.reset();
  CHECK(heard.closes == 1);
}

TEST_CASE("track: a blocking write takes as long as the audio it writes",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 1, 640));  // an 80 ms buffer
  REQUIRE(t->ok());
  const std::vector<std::int16_t> pcm = ramp(4000);  // 500 ms
  t->play();
  const auto t0 = Clock::now();
  CHECK(t->write(pcm.data(), 4000, true) == 4000);
  const double took = ms_since(t0);
  // 500 ms less what the buffer holds and the pump's lead: about 390 ms.
  // Returning at once would be under 50.
  CHECK(took > 300);
  CHECK(took < 1500);
  // And the position follows the clock, not the hand-over.
  CHECK(t->position() < 4000);
  CHECK(eventually(1500, [&] { return t->position() == 4000; }));
}

TEST_CASE("track: a write that does not block takes what fits",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 1, 100));
  const std::vector<std::int16_t> pcm = ramp(1000);
  // Not playing: nothing leaves the buffer.
  CHECK(t->write(pcm.data(), 60, false) == 60);
  CHECK(t->write(pcm.data() + 60, 60, false) == 40);
  CHECK(t->write(pcm.data() + 100, 60, false) == 0);
  CHECK(t->buffered_frames() == 100);
  // A blocking write to a track that is not playing does not wait for a
  // play() that may never come.
  const auto t0 = Clock::now();
  CHECK(t->write(pcm.data() + 100, 60, true) == 0);
  CHECK(ms_since(t0) < 200);
  CHECK(t->position() == 0);
  CHECK(heard_copy(heard).empty());
  CHECK(t->write(nullptr, 10, false) == 0);
  CHECK(t->write(pcm.data(), 0, false) == 0);
  CHECK(t->write(pcm.data(), -3, false) == 0);
}

TEST_CASE("track: pause keeps the buffer, flush discards it",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 1, 8000));
  const std::vector<std::int16_t> pcm = ramp(8000);
  CHECK(t->state() == Track::State::Stopped);
  CHECK(t->write(pcm.data(), 4000, false) == 4000);
  t->play();
  CHECK(t->state() == Track::State::Playing);
  REQUIRE(eventually(1000, [&] { return heard_copy(heard).size() >= 400; }));
  t->pause();
  CHECK(t->state() == Track::State::Paused);
  // The pump may be mid-step; give it one, then nothing more must arrive.
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const std::size_t at_pause = heard_copy(heard).size();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  CHECK(heard_copy(heard).size() == at_pause);
  CHECK(t->buffered_frames() == 4000 - static_cast<int>(at_pause));
  CHECK(t->position() == at_pause);

  // flush() while playing is ignored.
  t->play();
  t->flush();
  CHECK(t->buffered_frames() > 0);
  t->pause();
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  t->flush();
  CHECK(t->buffered_frames() == 0);
  CHECK(t->position() == 0);
  const std::size_t at_flush = heard_copy(heard).size();
  CHECK(at_flush < 4000);  // the rest was discarded, not played

  // What comes after a flush follows what was heard before it, with the
  // discarded part missing.
  CHECK(t->write(pcm.data() + 4000, 800, false) == 800);
  t->play();
  REQUIRE(eventually(
      2000, [&] { return heard_copy(heard).size() == at_flush + 800; }));
  const std::vector<std::int16_t> all = heard_copy(heard);
  CHECK(std::vector<std::int16_t>(all.begin(),
                                  all.begin() + static_cast<long>(at_flush)) ==
        std::vector<std::int16_t>(pcm.begin(),
                                  pcm.begin() + static_cast<long>(at_flush)));
  CHECK(std::vector<std::int16_t>(all.begin() + static_cast<long>(at_flush),
                                  all.end()) ==
        std::vector<std::int16_t>(pcm.begin() + 4000, pcm.begin() + 4800));
}

TEST_CASE("track: stop plays out what was written, then resets",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 1, 2000));
  const std::vector<std::int16_t> pcm = ramp(1600);
  t->play();
  CHECK(t->write(pcm.data(), 1600, false) == 1600);
  t->stop();
  CHECK(t->state() == Track::State::Stopped);  // as the app asked
  REQUIRE(eventually(2000, [&] { return heard_copy(heard).size() == 1600; }));
  CHECK(heard_copy(heard) == pcm);
  CHECK(eventually(500, [&] { return t->position() == 0; }));
  CHECK(t->buffered_frames() == 0);
  // And it plays again.
  t->play();
  CHECK(t->write(pcm.data(), 100, false) == 100);
  CHECK(eventually(1000, [&] { return heard_copy(heard).size() == 1700; }));
}

TEST_CASE("track: pause, stop, flush and release let a blocked writer go",
          "[audio][track]") {
  const std::vector<std::int16_t> pcm = ramp(16000);
  for (int how = 0; how < 4; ++how) {
    Heard heard;
    auto t = make(heard, config(8000, 1, 640));
    t->play();
    std::atomic<int> result{-100};
    std::thread writer([&] {
      result = t->write(pcm.data(), 16000, true);  // two seconds of audio
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(result == -100);  // still blocked
    const auto t0 = Clock::now();
    switch (how) {
      case 0:
        t->pause();
        break;
      case 1:
        t->stop();
        break;
      case 2:
        t->pause();
        t->flush();
        break;
      default:
        // Release while a write holds the track: the track is ended, and
        // destroyed once the write has returned.
        t->shutdown();
        break;
    }
    writer.join();
    INFO("how=" << how);
    CHECK(ms_since(t0) < 500);
    // It took part of what it was given, never all of it.
    CHECK(result.load() < 16000);
    CHECK(result.load() >= -1);
    if (how == 3) {
      // Ended for good.
      const std::int16_t one[] = {1};
      CHECK(t->write(one, 1, false) == -1);
    }
  }
}

TEST_CASE("track: running out of data is counted, and playback resumes",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 1, 2000));
  const std::vector<std::int16_t> pcm = ramp(1600);
  t->play();
  CHECK(t->underruns() == 0);
  CHECK(t->write(pcm.data(), 400, false) == 400);
  REQUIRE(eventually(1000, [&] { return heard_copy(heard).size() == 400; }));
  REQUIRE(eventually(500, [&] { return t->underruns() == 1; }));
  // Staying empty is one underrun, not one per step of the pump.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(t->underruns() == 1);
  // No burst when data comes back: 800 frames take about 100 ms to leave.
  const auto t0 = Clock::now();
  CHECK(t->write(pcm.data() + 400, 800, false) == 800);
  REQUIRE(eventually(1000, [&] { return heard_copy(heard).size() == 1200; }));
  CHECK(ms_since(t0) > 40);
  CHECK(heard_copy(heard) ==
        std::vector<std::int16_t>(pcm.begin(), pcm.begin() + 1200));
}

TEST_CASE("track: volume scales what the sink hears", "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 2, 400));
  const std::vector<std::int16_t> pcm = {1000,  1000,  -2000,
                                         -2000, 32767, -32768};
  t->set_volume(0.5f, 0.25f);
  t->play();
  CHECK(t->write(pcm.data(), 3, false) == 3);
  REQUIRE(eventually(1000, [&] { return heard_copy(heard).size() == 6; }));
  CHECK(heard_copy(heard) ==
        std::vector<std::int16_t>{500, 250, -1000, -500, 16384, -8192});

  // Out of range and not-a-number are silence or full, never louder.
  Heard h2;
  auto m = make(h2, config(8000, 1, 400));
  m->set_volume(7.0f, std::nanf(""));  // 1 and 0: a mono mean of 0.5
  m->play();
  const std::int16_t one[] = {1000};
  CHECK(m->write(one, 1, false) == 1);
  REQUIRE(eventually(1000, [&] { return heard_copy(h2).size() == 1; }));
  CHECK(heard_copy(h2)[0] == 500);
}

TEST_CASE("track: a static track holds a sound and plays it on demand",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 1, 800, /*is_static=*/true));
  const std::vector<std::int16_t> pcm = ramp(1000);
  // Writes fill the sound and stop at its size; they never block.
  CHECK(t->write(pcm.data(), 500, true) == 500);
  CHECK(t->write(pcm.data() + 500, 500, true) == 300);
  CHECK(t->write(pcm.data(), 10, true) == 0);
  CHECK(heard_copy(heard).empty());
  t->play();
  REQUIRE(eventually(2000, [&] { return heard_copy(heard).size() == 800; }));
  CHECK(heard_copy(heard) ==
        std::vector<std::int16_t>(pcm.begin(), pcm.begin() + 800));
  // At its end it stops by itself.
  CHECK(eventually(500, [&] { return t->state() == Track::State::Stopped; }));
  CHECK(t->underruns() == 0);
  // flush() does not discard a static sound.
  t->flush();
  CHECK(t->reload_static());
  t->play();
  CHECK_FALSE(t->reload_static());  // not while playing
  REQUIRE(eventually(2000, [&] { return heard_copy(heard).size() == 1600; }));

  // A streaming track has nothing to reload.
  Heard h2;
  CHECK_FALSE(make(h2, config(8000, 1, 800))->reload_static());
}

TEST_CASE("track: a timestamp exists once something has played",
          "[audio][track]") {
  Heard heard;
  auto t = make(heard, config(8000, 1, 2000));
  std::uint64_t frames = 99;
  std::int64_t ns = 0;
  CHECK_FALSE(t->timestamp(&frames, &ns));
  const std::vector<std::int16_t> pcm = ramp(1600);
  t->play();
  CHECK(t->write(pcm.data(), 1600, false) == 1600);
  REQUIRE(eventually(1000, [&] { return t->timestamp(&frames, &ns); }));
  CHECK(frames > 0);
  CHECK(frames <= 1600);
  CHECK(ns > 0);
  // Later it is further on, and never past what was written.
  std::uint64_t later = 0;
  std::int64_t later_ns = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(t->timestamp(&later, &later_ns));
  CHECK(later >= frames);
  CHECK(later <= 1600);
  CHECK(later_ns > ns);
}

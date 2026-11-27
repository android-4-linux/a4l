// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l tests. No GPL-3.0 sources.
//
// audio::Recorder over the silent source: bounds, pacing by the clock, what a
// late reader loses, and that stop and shutdown let a blocked read go.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "audio/audio_record.h"

namespace {

using acl::audio::make_silent_source;
using acl::audio::Recorder;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

Recorder::Config config(int rate, int channels, int capacity) {
  Recorder::Config c;
  c.sample_rate = rate;
  c.channels = channels;
  c.capacity_frames = capacity;
  return c;
}

}  // namespace

TEST_CASE("Recorder: bounds and the smallest buffer", "[audio][record]") {
  CHECK(Recorder::min_capacity_frames(48000) == 1920);
  CHECK(Recorder::min_capacity_frames(44100) == 1764);
  CHECK(Recorder::min_capacity_frames(3999) == 0);
  CHECK(Recorder::min_capacity_frames(192001) == 0);
  CHECK(Recorder::valid(config(16000, 1, 640)));
  CHECK_FALSE(Recorder::valid(config(16000, 0, 640)));
  CHECK_FALSE(Recorder::valid(config(16000, 9, 640)));
  CHECK_FALSE(Recorder::valid(config(16000, 1, 0)));
  CHECK_FALSE(Recorder::valid(config(1000, 1, 640)));

  const Recorder bad(config(16000, 0, 640), make_silent_source());
  CHECK_FALSE(bad.ok());
  const Recorder none(config(16000, 1, 640), nullptr);
  CHECK_FALSE(none.ok());
}

TEST_CASE("Recorder: a blocking read is paced and silent", "[audio][record]") {
  Recorder rec(config(48000, 2, 4800), make_silent_source());
  REQUIRE(rec.ok());
  CHECK_FALSE(rec.recording());
  rec.start();
  CHECK(rec.recording());

  // 30 ms of stereo. The buffer starts full of something else.
  std::vector<std::int16_t> pcm(1440 * 2, 0x1234);
  const Clock::time_point t0 = Clock::now();
  CHECK(rec.read(pcm.data(), 1440, true) == 1440);
  const auto took = Clock::now() - t0;
  CHECK(took >= milliseconds(25));
  CHECK(took < milliseconds(1000));
  CHECK(std::all_of(pcm.begin(), pcm.end(),
                    [](std::int16_t s) { return s == 0; }));
  CHECK(rec.position() == 1440);
}

TEST_CASE("Recorder: a read that does not block takes what is there",
          "[audio][record]") {
  Recorder rec(config(48000, 1, 4800), make_silent_source());
  REQUIRE(rec.ok());
  std::vector<std::int16_t> pcm(48000);
  // Not started: nothing.
  CHECK(rec.read(pcm.data(), 48000, false) == 0);
  rec.start();
  // A second's worth is not there yet.
  const int first = rec.read(pcm.data(), 48000, false);
  CHECK(first >= 0);
  CHECK(first < 4800);
}

TEST_CASE("Recorder: a late reader gets no more than the buffer",
          "[audio][record]") {
  // 10 ms of buffer, read after 60 ms.
  Recorder rec(config(48000, 1, 480), make_silent_source());
  REQUIRE(rec.ok());
  rec.start();
  std::this_thread::sleep_for(milliseconds(60));
  std::vector<std::int16_t> pcm(4800);
  CHECK(rec.read(pcm.data(), 4800, false) == 480);
}

TEST_CASE("Recorder: stop lets a blocked read go with what it has",
          "[audio][record]") {
  Recorder rec(config(48000, 1, 4800), make_silent_source());
  REQUIRE(rec.ok());
  rec.start();
  std::vector<std::int16_t> pcm(48000 * 5);
  auto reading = std::async(std::launch::async, [&] {
    return rec.read(pcm.data(), 48000 * 5, true);  // five seconds
  });
  std::this_thread::sleep_for(milliseconds(50));
  rec.stop();
  REQUIRE(reading.wait_for(milliseconds(2000)) == std::future_status::ready);
  const int got = reading.get();
  CHECK(got > 0);
  CHECK(got < 48000);
  CHECK_FALSE(rec.recording());
  // Stopped and emptied: a read returns nothing, at once.
  CHECK(rec.read(pcm.data(), 480, true) == 0);

  // It starts again.
  rec.start();
  CHECK(rec.read(pcm.data(), 480, true) == 480);
}

TEST_CASE("Recorder: shutdown ends a blocked read and later ones",
          "[audio][record]") {
  Recorder rec(config(48000, 1, 4800), make_silent_source());
  REQUIRE(rec.ok());
  rec.start();
  std::vector<std::int16_t> pcm(48000 * 5);
  auto reading = std::async(std::launch::async, [&] {
    return rec.read(pcm.data(), 48000 * 5, true);
  });
  std::this_thread::sleep_for(milliseconds(50));
  rec.shutdown();
  REQUIRE(reading.wait_for(milliseconds(2000)) == std::future_status::ready);
  CHECK(reading.get() == -1);
  CHECK(rec.read(pcm.data(), 480, false) == -1);
  rec.start();
  CHECK_FALSE(rec.recording());
}

namespace {

// Counts what is done to it and hands out a constant, so a reader can tell it
// from silence.
class FakeDevice final : public acl::audio::Source {
 public:
  struct Seen {
    int opens = 0;
    int starts = 0;
    int stops = 0;
    int closes = 0;
  };
  FakeDevice(Seen* seen, bool opens) : seen_(seen), opens_(opens) {}
  bool open(const acl::audio::Format&, int) override {
    ++seen_->opens;
    return opens_;
  }
  void start() override { ++seen_->starts; }
  void stop() override { ++seen_->stops; }
  int read(std::int16_t* pcm, int frames, bool) override {
    std::fill(pcm, pcm + frames, std::int16_t{1234});
    return frames;
  }
  void close() override { ++seen_->closes; }

 private:
  Seen* seen_;
  bool opens_;
};

acl::audio::Format mono16k() {
  acl::audio::Format f;
  f.sample_rate = 16000;
  f.channels = 1;
  return f;
}

}  // namespace

TEST_CASE("gated source: the device only while allowed", "[audio][record]") {
  FakeDevice::Seen seen;
  bool allowed = false;
  int asked = 0;
  auto source = acl::audio::make_gated_source(
      std::make_unique<FakeDevice>(&seen, true), [&] {
        ++asked;
        return allowed;
      });
  REQUIRE(source->open(mono16k(), 1600));
  CHECK(seen.opens == 1);
  CHECK(asked == 0);  // not asked until something records

  std::int16_t pcm[160];
  source->start();
  CHECK(asked == 1);
  CHECK(seen.starts == 0);
  std::this_thread::sleep_for(milliseconds(30));
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  CHECK(std::all_of(pcm, pcm + 160, [](std::int16_t v) { return v == 0; }));
  source->start();  // already started: not asked again
  CHECK(asked == 1);
  source->stop();
  CHECK(seen.stops == 0);

  // Allowed between two recordings: the second hears the device.
  allowed = true;
  source->start();
  CHECK(asked == 2);
  CHECK(seen.starts == 1);
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  CHECK(std::all_of(pcm, pcm + 160, [](std::int16_t v) { return v == 1234; }));

  // Revoked while recording: this recording keeps the device, the next has
  // silence.
  allowed = false;
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  CHECK(pcm[0] == 1234);
  source->stop();
  CHECK(seen.stops == 1);
  source->start();
  CHECK(asked == 3);
  CHECK(seen.starts == 1);
  std::this_thread::sleep_for(milliseconds(30));
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  CHECK(pcm[0] == 0);
  source->close();
  CHECK(seen.closes == 1);
}

TEST_CASE("gated source: no gate and a dead device are silence",
          "[audio][record]") {
  std::int16_t pcm[160];
  {
    FakeDevice::Seen seen;
    auto source = acl::audio::make_gated_source(
        std::make_unique<FakeDevice>(&seen, true), nullptr);
    REQUIRE(source->open(mono16k(), 1600));
    source->start();
    CHECK(seen.starts == 0);
    std::this_thread::sleep_for(milliseconds(30));
    REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
    CHECK(pcm[0] == 0);
  }
  {
    // The device does not open: the recorder still does, on silence, and the
    // gate is not asked for a device that is not there.
    FakeDevice::Seen seen;
    int asked = 0;
    auto source = acl::audio::make_gated_source(
        std::make_unique<FakeDevice>(&seen, false), [&] {
          ++asked;
          return true;
        });
    REQUIRE(source->open(mono16k(), 1600));
    source->start();
    CHECK(asked == 0);
    CHECK(seen.starts == 0);
    std::this_thread::sleep_for(milliseconds(30));
    REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
    CHECK(pcm[0] == 0);
  }
}

TEST_CASE("make_source: silence without ACL_AUDIO=pipewire and a gate",
          "[audio][record]") {
  // The test environment sets neither, and must not open a microphone.
  if (std::getenv("ACL_AUDIO") != nullptr)
    SKIP("ACL_AUDIO is set");
  auto source = acl::audio::make_source();
  REQUIRE(source->open(mono16k(), 1600));
  source->start();
  std::this_thread::sleep_for(milliseconds(30));
  std::int16_t pcm[160];
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  CHECK(std::all_of(pcm, pcm + 160, [](std::int16_t v) { return v == 0; }));
  source->close();
}

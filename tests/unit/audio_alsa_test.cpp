// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l tests. No GPL-3.0 sources.
//
// The ALSA sink and source against alsa-lib's "null" PCM, which takes and
// gives samples with no sound card: the calls are the real ones, the device is
// not. ACL_ALSA_DEVICE picks it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "audio/alsa_device.h"

using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace {

struct DeviceEnv {
  explicit DeviceEnv(const char* name) { ::setenv("ACL_ALSA_DEVICE", name, 1); }
  ~DeviceEnv() { ::unsetenv("ACL_ALSA_DEVICE"); }
};

}  // namespace

TEST_CASE("alsa: the device is 'default' unless ACL_ALSA_DEVICE names one",
          "[audio][alsa]") {
  ::unsetenv("ACL_ALSA_DEVICE");
  CHECK(acl::audio::alsa_device_name() == "default");
  ::setenv("ACL_ALSA_DEVICE", "", 1);
  CHECK(acl::audio::alsa_device_name() == "default");
  const DeviceEnv env("plughw:7,0");
  CHECK(acl::audio::alsa_device_name() == "plughw:7,0");
}

TEST_CASE("alsa sink: samples written are handed to the device and counted",
          "[audio][alsa]") {
  const DeviceEnv env("null");
  auto sink = acl::audio::make_alsa_sink();
  REQUIRE(sink->open({48000, 2}));
  const std::vector<std::int16_t> pcm(48000 * 2 / 10, 1000);  // 100 ms
  for (int i = 0; i < 5; ++i)
    REQUIRE(sink->write(pcm.data(), static_cast<int>(pcm.size())) ==
            static_cast<int>(pcm.size()));
  CHECK(sink->frames_written() == 5u * 4800u);
  sink->drain();
  CHECK(sink->frames_pending() == 0u);
  CHECK(sink->latency_ns() >= 0);  // read once the device has run
  sink->close();
  sink->close();  // a second close is harmless
  CHECK(sink->frames_written() == 5u * 4800u);
}

TEST_CASE("alsa sink: a device that cannot be opened fails open()",
          "[audio][alsa]") {
  const DeviceEnv env("a4l-no-such-pcm");
  auto sink = acl::audio::make_alsa_sink();
  CHECK_FALSE(sink->open({48000, 2}));
  std::int16_t one[2] = {0, 0};
  CHECK(sink->write(one, 2) == 0);
  sink->drain();
  sink->close();
}

TEST_CASE("alsa sink: close with samples still queued returns promptly",
          "[audio][alsa]") {
  const DeviceEnv env("null");
  auto sink = acl::audio::make_alsa_sink();
  REQUIRE(sink->open({44100, 1}));
  const std::vector<std::int16_t> pcm(44100, 0);  // a second
  sink->write(pcm.data(), static_cast<int>(pcm.size()));
  const auto t0 = steady_clock::now();
  sink->close();
  CHECK(steady_clock::now() - t0 < milliseconds(1000));
}

TEST_CASE("alsa source: a started source delivers frames from the device",
          "[audio][alsa]") {
  const DeviceEnv env("null");
  auto source = acl::audio::make_alsa_source();
  REQUIRE(source->open({16000, 1}, 1600));
  std::int16_t pcm[160];
  CHECK(source->read(pcm, 160, /*blocking=*/false) == 0);  // not started
  source->start();
  std::fill(pcm, pcm + 160, std::int16_t{7});
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  // The null PCM records silence.
  CHECK(std::all_of(pcm, pcm + 160, [](std::int16_t v) { return v == 0; }));
  const auto t0 = steady_clock::now();
  source->stop();
  CHECK(steady_clock::now() - t0 < milliseconds(1000));
  source->start();  // and again: the device is opened per recording
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  source->close();
}

TEST_CASE("alsa source: with no device a recording is silence, in real time",
          "[audio][alsa]") {
  const DeviceEnv env("a4l-no-such-pcm");
  auto source = acl::audio::make_alsa_source();
  REQUIRE(source->open({16000, 1}, 1600));
  source->start();
  std::this_thread::sleep_for(milliseconds(30));
  std::int16_t pcm[160];
  std::fill(pcm, pcm + 160, std::int16_t{7});
  REQUIRE(source->read(pcm, 160, /*blocking=*/true) == 160);
  CHECK(std::all_of(pcm, pcm + 160, [](std::int16_t v) { return v == 0; }));
  source->close();
}

TEST_CASE("alsa volume: decibels to a linear amplitude and back",
          "[audio][alsa]") {
  using acl::audio::alsa_db_from_linear;
  using acl::audio::alsa_linear_from_db;
  // A control from -102.39 dB to +4.00 dB, as a Raspberry Pi 4's "PCM" is.
  const long lo = -10239, hi = 400;
  CHECK(alsa_linear_from_db(hi, hi) == 1.0f);
  CHECK(alsa_linear_from_db(hi + 100, hi) == 1.0f);
  // 20 dB under the top is a tenth of the amplitude; 6.02 dB is about half.
  CHECK(std::abs(alsa_linear_from_db(hi - 2000, hi) - 0.1f) < 1e-4f);
  CHECK(std::abs(alsa_linear_from_db(hi - 602, hi) - 0.5f) < 1e-3f);
  CHECK(alsa_db_from_linear(1.0f, lo, hi) == hi);
  CHECK(alsa_db_from_linear(2.0f, lo, hi) == hi);
  CHECK(alsa_db_from_linear(0.0f, lo, hi) == lo);
  CHECK(alsa_db_from_linear(-1.0f, lo, hi) == lo);
  CHECK(alsa_db_from_linear(0.1f, lo, hi) == hi - 2000);
  CHECK(alsa_db_from_linear(0.5f, lo, hi) == hi - 602);
  // Quieter than the control goes is its bottom.
  CHECK(alsa_db_from_linear(1e-9f, lo, hi) == lo);
}

TEST_CASE("alsa volume: a device with no mixer reports full volume, fixed",
          "[audio][alsa]") {
  const DeviceEnv env("null");
  auto v = acl::audio::make_alsa_volume();
  REQUIRE(v != nullptr);
  CHECK_FALSE(v->available());
  CHECK(v->volume() == 1.0f);
  CHECK_FALSE(v->muted());
  CHECK_FALSE(v->set_volume(0.5f));
  CHECK_FALSE(v->set_muted(true));
  CHECK(v->volume() == 1.0f);
}

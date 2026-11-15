// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test; the android.media.MediaCodec buffer model as
// the SDK documents it. No GPL-3.0 sources.
//
// media::AudioCodec, the byte-buffer model under android.media.MediaCodec for
// audio, against a decoder made for the test: every access unit comes out as
// PCM once and in order, the format is announced before the first buffer and
// on a change, the stream ends with one flagged buffer, and neither a client
// that queues without reading nor one that reads without releasing makes it
// hold more than its buffers.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

#include "media/audio_codec.h"

using namespace acl;
using media::AudioCodec;
using media::AudioDecoder;
using Kind = AudioCodec::Output::Kind;

namespace {

// A "decoder" whose access unit is: one byte of channel count, one byte of
// sample rate in kHz, then 16-bit samples, which it hands back unchanged.
// Like a real one it holds a single frame: a second unit is refused until
// the first has been read.
struct FakeDecoder {
  std::deque<AudioDecoder::Frame> ready;
  bool eos_sent = false;
  int sends = 0, flushes = 0;
  bool broken = false;   // asks for input and refuses it
  bool failing = false;  // receive() reports an error

  AudioCodec::Decoder ops() {
    AudioCodec::Decoder d;
    d.send = [this](const std::uint8_t* data, std::size_t size,
                    std::int64_t pts) {
      if (broken)
        return 1;
      if (!ready.empty())
        return 1;
      ++sends;
      if (data == nullptr) {
        eos_sent = true;
        return 0;
      }
      if (size < 2)
        return 0;  // undecodable: dropped
      AudioDecoder::Frame f;
      f.channels = data[0];
      f.sample_rate = data[1] * 1000;
      f.pts_us = pts;
      f.pcm.resize((size - 2) / 2);
      std::memcpy(f.pcm.data(), data + 2, f.pcm.size() * 2);
      ready.push_back(std::move(f));
      return 0;
    };
    d.receive = [this](AudioDecoder::Frame* out) {
      if (failing)
        return AudioDecoder::Result::Error;
      if (!ready.empty()) {
        *out = std::move(ready.front());
        ready.pop_front();
        return AudioDecoder::Result::Frame;
      }
      return eos_sent ? AudioDecoder::Result::EndOfStream
                      : AudioDecoder::Result::Again;
    };
    d.flush = [this] {
      ready.clear();
      eos_sent = false;
      ++flushes;
    };
    return d;
  }
};

AudioCodec::Limits limits(int in, int out, std::size_t bytes = 64) {
  AudioCodec::Limits l;
  l.input_buffers = in;
  l.output_buffers = out;
  l.input_bytes = bytes;
  return l;
}

// Queue an access unit of `samples` at `rate_khz`, `channels`.
bool queue(AudioCodec& c,
           int index,
           const std::vector<std::int16_t>& samples,
           std::int64_t pts,
           bool eos = false,
           int channels = 2,
           int rate_khz = 48) {
  std::uint8_t* mem = c.input_data(index);
  REQUIRE(mem != nullptr);
  mem[0] = static_cast<std::uint8_t>(channels);
  mem[1] = static_cast<std::uint8_t>(rate_khz);
  std::memcpy(mem + 2, samples.data(), samples.size() * 2);
  return c.queue_input(index, 0, static_cast<int>(2 + samples.size() * 2), pts,
                       eos);
}

std::vector<std::int16_t> pcm_of(const AudioCodec& c, int index) {
  std::size_t bytes = 0;
  const std::int16_t* p = c.output_data(index, &bytes);
  REQUIRE(p != nullptr);
  return {p, p + bytes / 2};
}

}  // namespace

TEST_CASE("audio codec: units come out as PCM, once and in order",
          "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(4, 8));
  CHECK(c.input_buffers() == 4);
  CHECK(c.input_bytes() == 64);
  // Nothing yet.
  CHECK(c.dequeue_output().kind == Kind::TryAgain);
  CHECK(c.sample_rate() == 0);

  const int a = c.dequeue_input();
  const int b = c.dequeue_input();
  REQUIRE(a == 0);
  REQUIRE(b == 1);
  REQUIRE(queue(c, a, {1, 2, 3, 4}, 1000));
  REQUIRE(queue(c, b, {5, 6}, 2000));

  // The format first, then the buffers.
  AudioCodec::Output o = c.dequeue_output();
  CHECK(o.kind == Kind::FormatChanged);
  CHECK(c.sample_rate() == 48000);
  CHECK(c.channels() == 2);
  o = c.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  CHECK(o.bytes == 8);
  CHECK(o.pts_us == 1000);
  CHECK_FALSE(o.end_of_stream);
  CHECK(pcm_of(c, o.index) == std::vector<std::int16_t>{1, 2, 3, 4});
  const int first = o.index;
  o = c.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  CHECK(o.index != first);  // the first is still the client's
  CHECK(o.pts_us == 2000);
  CHECK(pcm_of(c, o.index) == std::vector<std::int16_t>{5, 6});
  CHECK(c.dequeue_output().kind == Kind::TryAgain);
  c.release_output(first);
  c.release_output(o.index);
  CHECK(c.output_data(first, nullptr) == nullptr);
  // Both input buffers are the client's to take again.
  CHECK(c.dequeue_input() >= 0);
  CHECK(c.dequeue_input() >= 0);
  CHECK(dec.sends == 2);
}

TEST_CASE("audio codec: the stream ends with one flagged, empty buffer",
          "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(4, 8));
  REQUIRE(queue(c, c.dequeue_input(), {9, 9}, 10));
  // End of stream on its own buffer, with no data.
  const int e = c.dequeue_input();
  REQUIRE(c.queue_input(e, 0, 0, 20, true));
  // Nothing may follow the end of the stream.
  CHECK(c.dequeue_input() == -1);

  CHECK(c.dequeue_output().kind == Kind::FormatChanged);
  AudioCodec::Output o = c.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  CHECK_FALSE(o.end_of_stream);
  c.release_output(o.index);
  o = c.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  CHECK(o.end_of_stream);
  CHECK(o.bytes == 0);
  c.release_output(o.index);
  // Once.
  CHECK(c.dequeue_output().kind == Kind::TryAgain);
  CHECK(c.dequeue_output().kind == Kind::TryAgain);

  // End of stream on the last access unit itself.
  FakeDecoder dec2;
  AudioCodec c2(dec2.ops(), limits(4, 8));
  REQUIRE(queue(c2, c2.dequeue_input(), {7}, 5, /*eos=*/true, 1));
  CHECK(c2.dequeue_output().kind == Kind::FormatChanged);
  o = c2.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  CHECK(pcm_of(c2, o.index) == std::vector<std::int16_t>{7});
  CHECK_FALSE(o.end_of_stream);
  o = c2.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  CHECK(o.end_of_stream);
}

TEST_CASE("audio codec: a change of format is announced before its buffer",
          "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(4, 8));
  REQUIRE(queue(c, c.dequeue_input(), {1, 1}, 0, false, 2, 48));
  REQUIRE(queue(c, c.dequeue_input(), {2, 2}, 1, false, 2, 48));
  REQUIRE(queue(c, c.dequeue_input(), {3}, 2, false, 1, 44));
  std::vector<Kind> kinds;
  std::vector<int> rates;
  for (int i = 0; i < 8; ++i) {
    const AudioCodec::Output o = c.dequeue_output();
    if (o.kind == Kind::TryAgain)
      break;
    kinds.push_back(o.kind);
    rates.push_back(c.sample_rate());
    if (o.kind == Kind::Buffer)
      c.release_output(o.index);
  }
  CHECK(kinds == std::vector<Kind>{Kind::FormatChanged, Kind::Buffer,
                                   Kind::Buffer, Kind::FormatChanged,
                                   Kind::Buffer});
  CHECK(rates == std::vector<int>{48000, 48000, 48000, 44000, 44000});
  CHECK(c.channels() == 1);
}

TEST_CASE("audio codec: it holds no more than its buffers", "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(3, 2));
  // A client that queues and never reads runs out of input buffers.
  for (int i = 0; i < 3; ++i) {
    const int idx = c.dequeue_input();
    REQUIRE(idx >= 0);
    REQUIRE(queue(c, idx, {static_cast<std::int16_t>(i)}, i, false, 1));
  }
  CHECK(c.dequeue_input() == -1);
  CHECK(dec.sends == 0);  // nothing is decoded until output is asked for

  // A client that reads and never releases runs out of output buffers.
  CHECK(c.dequeue_output().kind == Kind::FormatChanged);
  const AudioCodec::Output o1 = c.dequeue_output();
  const AudioCodec::Output o2 = c.dequeue_output();
  REQUIRE(o1.kind == Kind::Buffer);
  REQUIRE(o2.kind == Kind::Buffer);
  CHECK(c.dequeue_output().kind == Kind::TryAgain);
  CHECK(c.dequeue_output().kind == Kind::TryAgain);
  // The third unit has been decoded and waits, so its input buffer is free;
  // the frame is not lost.
  c.release_output(o1.index);
  const AudioCodec::Output o3 = c.dequeue_output();
  REQUIRE(o3.kind == Kind::Buffer);
  CHECK(pcm_of(c, o3.index) == std::vector<std::int16_t>{2});
  CHECK(pcm_of(c, o2.index) == std::vector<std::int16_t>{1});
}

TEST_CASE("audio codec: what the client may not queue", "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(2, 2, 16));
  // Not dequeued, out of range.
  CHECK_FALSE(c.queue_input(0, 0, 4, 0, false));
  CHECK_FALSE(c.queue_input(-1, 0, 4, 0, false));
  CHECK_FALSE(c.queue_input(2, 0, 4, 0, false));
  CHECK(c.input_data(-1) == nullptr);
  CHECK(c.input_data(2) == nullptr);
  const int i = c.dequeue_input();
  REQUIRE(i == 0);
  // A range outside the buffer, each way. The buffer stays the client's.
  CHECK_FALSE(c.queue_input(i, -1, 4, 0, false));
  CHECK_FALSE(c.queue_input(i, 0, -1, 0, false));
  CHECK_FALSE(c.queue_input(i, 0, 17, 0, false));
  CHECK_FALSE(c.queue_input(i, 16, 1, 0, false));
  CHECK_FALSE(c.queue_input(i, 17, 0, 0, false));
  CHECK_FALSE(c.queue_input(i, 0x7fffffff, 0x7fffffff, 0, false));
  CHECK(c.queue_input(i, 16, 0, 0, false));  // empty, at the very end
  // Twice.
  CHECK_FALSE(c.queue_input(i, 0, 4, 0, false));
  // Output indices the client does not hold.
  CHECK(c.output_data(0, nullptr) == nullptr);
  CHECK(c.output_data(-1, nullptr) == nullptr);
  CHECK(c.output_data(99, nullptr) == nullptr);
  c.release_output(-1);
  c.release_output(99);
  c.release_output(0);
}

TEST_CASE("audio codec: flush drops what is in flight and the stream restarts",
          "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(2, 2));
  REQUIRE(queue(c, c.dequeue_input(), {1}, 0, false, 1));
  const int held_in = c.dequeue_input();
  REQUIRE(held_in >= 0);
  CHECK(c.dequeue_output().kind == Kind::FormatChanged);
  const AudioCodec::Output o = c.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  REQUIRE(queue(c, held_in, {2}, 1, /*eos=*/true, 1));

  c.flush();
  CHECK(dec.flushes == 1);
  // The held output is gone, both inputs are free, and the end of stream
  // that was queued is forgotten.
  CHECK(c.output_data(o.index, nullptr) == nullptr);
  CHECK(c.dequeue_output().kind == Kind::TryAgain);
  const int a = c.dequeue_input();
  const int b = c.dequeue_input();
  CHECK(a >= 0);
  CHECK(b >= 0);
  REQUIRE(queue(c, a, {3}, 2, false, 1));
  // The format is the same as before, so it is not announced again.
  const AudioCodec::Output after = c.dequeue_output();
  REQUIRE(after.kind == Kind::Buffer);
  CHECK(pcm_of(c, after.index) == std::vector<std::int16_t>{3});
}

TEST_CASE("audio codec: a decoder that fails ends the stream",
          "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(2, 2));
  REQUIRE(queue(c, c.dequeue_input(), {1}, 0, false, 1));
  dec.failing = true;
  const AudioCodec::Output o = c.dequeue_output();
  REQUIRE(o.kind == Kind::Buffer);
  CHECK(o.end_of_stream);  // told, not left waiting

  // One that asks for input and refuses it is not spun on.
  FakeDecoder stuck;
  stuck.broken = true;
  AudioCodec c2(stuck.ops(), limits(2, 2));
  REQUIRE(queue(c2, c2.dequeue_input(), {1}, 0, false, 1));
  CHECK(c2.dequeue_output().kind == Kind::TryAgain);
  CHECK(c2.dequeue_output().kind == Kind::TryAgain);
}

TEST_CASE("audio codec: a driver is woken by input, release and wake",
          "[media][codec]") {
  FakeDecoder dec;
  AudioCodec c(dec.ops(), limits(2, 2));
  // With nothing to do it waits out its time.
  const auto t0 = std::chrono::steady_clock::now();
  c.wait_for_work(60);
  CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(50));
  // Work that arrived before the wait is not missed.
  REQUIRE(queue(c, c.dequeue_input(), {1}, 0, false, 1));
  const auto t1 = std::chrono::steady_clock::now();
  c.wait_for_work(2000);
  CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(500));
  // And one that arrives during it ends it.
  std::thread waker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    c.wake();
  });
  const auto t2 = std::chrono::steady_clock::now();
  c.wait_for_work(2000);
  CHECK(std::chrono::steady_clock::now() - t2 <
        std::chrono::milliseconds(1000));
  waker.join();
}

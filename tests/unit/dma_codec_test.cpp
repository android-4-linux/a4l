// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test over dmabuf-codec's public decoder interface
// (src/dma_decoder.h; MIT). No GPL-3.0 sources.
//
// media::DmaCodec against a decoder made for the test: frames come out once
// and in order with their timestamps, a frame's buffer goes back to the
// decoder when the last copy of the frame is dropped and not before, on any
// thread, a frame outlives the decoder it came from, a flush with frames in
// use replaces the decoder, and a change of geometry replaces it and sends
// again what it had not finished.

#include <catch2/catch_test_macros.hpp>

#include "acl_config.h"

#if ACL_HAVE_DMABUF_CODEC

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "media/dma_codec.h"
#include "src/dma_decoder.h"

using namespace acl;
using media::DmaCodec;

namespace {

constexpr std::uint32_t kNv12 = 0x3231564e;
constexpr std::uint32_t kW = 64, kH = 32, kPool = 4;

// What the test sees of every decoder the codec has made.
struct World {
  int made = 0;
  int alive = 0;
  int flushes = 0;
  int drains = 0;
  std::vector<std::string> units;       // every unit accepted, in order
  std::vector<std::uint32_t> released;  // buffer indices returned
  bool change_on_next = false;          // the next unit outgrows the decoder
  bool split_planes = false;            // hand out a frame as two buffers
};

// One access unit in, one frame out, from a pool of kPool buffers. A unit is
// refused while no buffer is free.
class FakeDecoder final : public v4l2wc::IDmaDecoder {
 public:
  explicit FakeDecoder(World* w) : w_(w) {
    ++w_->made;
    ++w_->alive;
    for (std::uint32_t i = 0; i < kPool; ++i) {
      const int fd = ::memfd_create("dma-codec-test", MFD_CLOEXEC);
      REQUIRE(fd >= 0);
      REQUIRE(::ftruncate(fd, kW * kH * 3 / 2) == 0);
      fds_.push_back(fd);
      free_.push_back(i);
    }
    other_ = ::memfd_create("dma-codec-test", MFD_CLOEXEC);
  }
  ~FakeDecoder() override {
    --w_->alive;
    for (const int fd : fds_)
      ::close(fd);
    ::close(other_);
  }
  v4l2wc::SubmitResult SubmitBitstream(const std::uint8_t* data,
                                       std::size_t size,
                                       std::uint64_t timestamp) override {
    if (changed_)
      return v4l2wc::SubmitResult::kSourceChange;
    if (w_->change_on_next || change_queued_) {
      // As a stateful decoder does: the unit that outgrows it is taken, and
      // so is what follows, and none of it comes out. The change is reported
      // from Drive().
      w_->change_on_next = false;
      w_->units.emplace_back(data, data + size);
      change_queued_ = true;
      return v4l2wc::SubmitResult::kOk;
    }
    if (free_.empty())
      return v4l2wc::SubmitResult::kTryAgain;
    w_->units.emplace_back(data, data + size);
    ready_.push_back({free_.front(), timestamp});
    free_.pop_front();
    return v4l2wc::SubmitResult::kOk;
  }
  v4l2wc::DriveResult Drive() override {
    if (change_queued_)
      changed_ = true;
    return changed_ ? v4l2wc::DriveResult::kSourceChange
                    : v4l2wc::DriveResult::kOk;
  }
  bool Acquire(v4l2wc::V4l2DmaFrame* out) override {
    if (ready_.empty())
      return false;
    *out = v4l2wc::V4l2DmaFrame{};
    out->capture_index = ready_.front().first;
    out->timestamp = ready_.front().second;
    ready_.pop_front();
    out->width = kW;
    out->height = kH;
    out->drm_fourcc = kNv12;
    out->num_planes = 2;
    out->fds[0] = fds_[out->capture_index];
    out->fds[1] = w_->split_planes ? other_ : out->fds[0];
    out->pitches[0] = out->pitches[1] = kW;
    out->offsets[1] = kW * kH;
    return true;
  }
  void Release(std::uint32_t index) override {
    w_->released.push_back(index);
    free_.push_back(index);
  }
  void Flush() override {
    ++w_->flushes;
    ready_.clear();
  }
  void Drain() override { ++w_->drains; }
  std::uint32_t PoolSize() const override { return kPool; }

 private:
  World* w_;
  std::vector<int> fds_;
  int other_ = -1;
  std::deque<std::uint32_t> free_;
  std::deque<std::pair<std::uint32_t, std::uint64_t>> ready_;
  bool change_queued_ = false;
  bool changed_ = false;
};

std::unique_ptr<DmaCodec> make(World* w, const std::string& csd = "") {
  std::string error;
  auto codec = DmaCodec::with_factory(
      [w] { return std::make_unique<FakeDecoder>(w); },
      reinterpret_cast<const std::uint8_t*>(csd.data()), csd.size(), &error);
  REQUIRE(codec != nullptr);
  return codec;
}

int send(DmaCodec& c, const std::string& unit, std::int64_t pts) {
  return c.send_packet(reinterpret_cast<const std::uint8_t*>(unit.data()),
                       unit.size(), pts, nullptr);
}

}  // namespace

TEST_CASE("DmaCodec: frames come out in order with their timestamps",
          "[media][dma]") {
  World w;
  auto codec = make(&w, "csd:");
  REQUIRE(send(*codec, "a", 100) == 0);
  REQUIRE(send(*codec, "b", 200) == 0);
  // The configured parameter sets go ahead of the first unit only.
  REQUIRE(w.units == std::vector<std::string>{"csd:a", "b"});

  gfx::DmabufFrame f;
  std::int64_t pts = 0;
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 100);
  CHECK(f.width == kW);
  CHECK(f.height == kH);
  CHECK(f.drm_format == kNv12);
  CHECK(f.plane_count == 2);
  CHECK(f.planes[1].offset == kW * kH);
  CHECK(f.mem_size == kW * kH * 3 / 2);
  CHECK(f.hold != nullptr);
  // The frame's descriptor is its own, not the decoder's.
  CHECK(::fcntl(f.fd, F_GETFD) >= 0);
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 200);
  CHECK(codec->receive_frame(&f, &pts, nullptr) == 0);

  CHECK(codec->send_packet(nullptr, 0, 0, nullptr) == 0);
  CHECK(w.drains == 1);
}

TEST_CASE("DmaCodec: a buffer goes back when the last copy of its frame does",
          "[media][dma]") {
  World w;
  auto codec = make(&w);
  for (int i = 0; i < static_cast<int>(kPool); ++i)
    REQUIRE(send(*codec, "u", i) == 0);
  // Every buffer is spoken for: the next unit waits.
  REQUIRE(send(*codec, "u", 99) == 1);

  std::vector<gfx::DmabufFrame> held(kPool);
  for (auto& f : held)
    REQUIRE(codec->receive_frame(&f, nullptr, nullptr) == 1);
  REQUIRE(send(*codec, "u", 99) == 1);
  CHECK(w.released.empty());

  gfx::DmabufFrame copy = held[1];
  held[1] = gfx::DmabufFrame{};
  CHECK(w.released.empty());  // the copy still has it
  // Dropped on another thread, as a compositor's release is.
  std::thread([c = std::move(copy)]() mutable {
    c = gfx::DmabufFrame{};
  }).join();
  REQUIRE(w.released == std::vector<std::uint32_t>{1});
  CHECK(send(*codec, "u", 99) == 0);

  // Receiving into a frame that is still held drops that frame first.
  REQUIRE(codec->receive_frame(&held[0], nullptr, nullptr) == 1);
  CHECK(w.released == std::vector<std::uint32_t>{1, 0});
}

TEST_CASE("DmaCodec: a frame outlives its decoder", "[media][dma]") {
  World w;
  gfx::DmabufFrame f;
  {
    auto codec = make(&w);
    REQUIRE(send(*codec, "u", 1) == 0);
    REQUIRE(codec->receive_frame(&f, nullptr, nullptr) == 1);
  }
  CHECK(w.alive == 0);  // the decoder went with the codec
  // The memory is still there to read, and letting go of it reaches nothing.
  CHECK(::lseek(f.fd, 0, SEEK_END) == kW * kH * 3 / 2);
  f = gfx::DmabufFrame{};
  CHECK(w.released.empty());
}

TEST_CASE("DmaCodec: flush", "[media][dma]") {
  World w;
  auto codec = make(&w, "csd:");
  gfx::DmabufFrame f;

  SECTION("with no frame in use the decoder is flushed") {
    REQUIRE(send(*codec, "a", 1) == 0);
    codec->flush();
    CHECK(w.flushes == 1);
    CHECK(w.made == 1);
    CHECK(codec->receive_frame(&f, nullptr, nullptr) == 0);
  }
  SECTION("with a frame in use the decoder is replaced") {
    REQUIRE(send(*codec, "a", 1) == 0);
    REQUIRE(codec->receive_frame(&f, nullptr, nullptr) == 1);
    codec->flush();
    CHECK(w.flushes == 0);
    CHECK(w.made == 2);
    CHECK(w.alive == 1);
    // The old frame's buffer belongs to no decoder now.
    f = gfx::DmabufFrame{};
    CHECK(w.released.empty());
  }
  // Either way the parameter sets lead the next unit again.
  REQUIRE(send(*codec, "k", 2) == 0);
  CHECK(w.units.back() == "csd:k");
  std::int64_t pts = 0;
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 2);
}

TEST_CASE("DmaCodec: a change of geometry replaces the decoder",
          "[media][dma]") {
  World w;
  auto codec = make(&w, "csd:");
  REQUIRE(send(*codec, "a", 1) == 0);
  REQUIRE(send(*codec, "b", 2) == 0);
  w.change_on_next = true;
  REQUIRE(send(*codec, "K", 3) == 0);  // taken; the change shows later
  REQUIRE(send(*codec, "d", 4) == 0);  // taken, and lost with it

  // What the old decoder had decoded still comes out, then it is replaced
  // and the units it had not finished go to the new one, without the
  // configured parameter sets: the stream has its own from here.
  gfx::DmabufFrame f;
  std::int64_t pts = 0;
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 1);
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 2);
  CHECK(w.made == 1);
  w.units.clear();
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 3);
  CHECK(w.made == 2);
  CHECK(w.alive == 1);
  CHECK(w.units == std::vector<std::string>{"K", "d"});
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 4);
  // And it carries on from there.
  REQUIRE(send(*codec, "e", 5) == 0);
  REQUIRE(codec->receive_frame(&f, &pts, nullptr) == 1);
  CHECK(pts == 5);
}

TEST_CASE(
    "DmaCodec: a frame in two buffers is refused, and its buffer "
    "returned",
    "[media][dma]") {
  World w;
  w.split_planes = true;
  auto codec = make(&w);
  REQUIRE(send(*codec, "u", 1) == 0);
  gfx::DmabufFrame f;
  std::string error;
  CHECK(codec->receive_frame(&f, nullptr, &error) == -1);
  CHECK_FALSE(error.empty());
  CHECK(w.released == std::vector<std::uint32_t>{0});
}

TEST_CASE("DmaCodec: no engine, no codec", "[media][dma]") {
  std::string error;
  CHECK(DmaCodec::with_factory([] { return nullptr; }, nullptr, 0, &error) ==
        nullptr);
  CHECK_FALSE(error.empty());
  CHECK(DmaCodec::configure("video/x-unknown", nullptr, 0, 64, 64, &error) ==
        nullptr);
}

#endif  // ACL_HAVE_DMABUF_CODEC

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test; the SPSs are libx264's (High profile, 64x64)
// for the color tags named. No GPL-3.0 sources.
//
// media::h264_color reads the matrix and range an H.264 SPS says, and treats
// the bytes as untrusted: a cut or garbled SPS says nothing and reads nothing
// past its end.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "media/h264_color.h"

using acl::media::h264_color;
using acl::media::H264Color;

namespace {

// colorprim/transfer/colormatrix=bt709, limited range. Has an emulation
// prevention byte (00 00 03).
const std::vector<std::uint8_t> kBt709 = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x0a, 0xac, 0xd9, 0x44,
    0x26, 0xc0, 0x5a, 0x80, 0x80, 0x80, 0xa0, 0x00, 0x00, 0x03, 0x00,
    0x20, 0x00, 0x00, 0x06, 0x41, 0xe2, 0x44, 0xb2, 0xc0};

// The same with colormatrix=bt470bg and full range.
std::vector<std::uint8_t> bt601_full() {
  std::vector<std::uint8_t> v = kBt709;
  v[13] = 0x5b;
  v[14] = v[15] = v[16] = 0x82;
  return v;
}

// A Baseline SPS with no VUI.
const std::vector<std::uint8_t> kNoVui = {0x00, 0x00, 0x01, 0x67, 0x42, 0x00,
                                          0x0a, 0xf8, 0x58, 0x98, 0x80};

}  // namespace

TEST_CASE("h264_color reads what the SPS says", "[media]") {
  H264Color c;
  REQUIRE(h264_color(kBt709.data(), kBt709.size(), &c));
  CHECK(c.matrix == 1);
  CHECK_FALSE(c.full_range);

  const auto full = bt601_full();
  REQUIRE(h264_color(full.data(), full.size(), &c));
  CHECK(c.matrix == 5);
  CHECK(c.full_range);
}

TEST_CASE("h264_color finds the SPS behind other units", "[media]") {
  // An access unit delimiter, then the SPS, then a PPS and an IDR slice.
  std::vector<std::uint8_t> au = {0x00, 0x00, 0x00, 0x01, 0x09, 0xf0};
  const auto full = bt601_full();
  au.insert(au.end(), full.begin(), full.end());
  for (std::uint8_t b : {0x00, 0x00, 0x01, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0,
                         0x00, 0x00, 0x01, 0x65, 0x88, 0x84})
    au.push_back(b);
  H264Color c;
  REQUIRE(h264_color(au.data(), au.size(), &c));
  CHECK(c.matrix == 5);
  CHECK(c.full_range);
}

TEST_CASE("h264_color says nothing where the stream does not", "[media]") {
  H264Color c;
  c.matrix = 9;
  CHECK_FALSE(h264_color(kNoVui.data(), kNoVui.size(), &c));
  CHECK(c.matrix == 9);  // untouched

  // A slice ahead of the SPS: the SPS is not looked for past it.
  std::vector<std::uint8_t> late = {0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
  late.insert(late.end(), kBt709.begin(), kBt709.end());
  CHECK_FALSE(h264_color(late.data(), late.size(), &c));

  CHECK_FALSE(h264_color(nullptr, 0, &c));
  CHECK_FALSE(h264_color(kBt709.data(), kBt709.size(), nullptr));
}

TEST_CASE("h264_color reads nothing past a cut SPS", "[media]") {
  // Every cut short of the matrix's last bit (in the 18th byte) says nothing;
  // none reads past the end (the sanitizer build checks that).
  const auto full = bt601_full();
  for (std::size_t n = 0; n < 18; ++n) {
    const std::vector<std::uint8_t> cut(full.begin(), full.begin() + n);
    H264Color c;
    CHECK_FALSE(h264_color(cut.data(), cut.size(), &c));
  }
  {
    H264Color c;
    CHECK(h264_color(full.data(), 18, &c));
    CHECK(c.matrix == 5);
  }
  // Garbled: every byte of the SPS flipped in turn. Says something or not,
  // but stays inside the buffer.
  for (std::size_t i = 5; i < full.size(); ++i) {
    std::vector<std::uint8_t> bad = full;
    bad[i] ^= 0xff;
    H264Color c;
    (void)h264_color(bad.data(), bad.size(), &c);
  }
  const std::vector<std::uint8_t> zeros(64, 0x00);
  H264Color c;
  CHECK_FALSE(h264_color(zeros.data(), zeros.size(), &c));
}

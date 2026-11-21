// SPDX-License-Identifier: Apache-2.0
// Provenance: ITU-T H.264 clauses 7.3.2.1.1, 7.3.2.1.1.1 and E.1.1. No GPL-3.0
// sources.

#include "media/h264_color.h"

#include <vector>

namespace acl::media {
namespace {

// The longest SPS read; a real one is a few dozen bytes.
constexpr std::size_t kMaxSps = 1024;

class Bits {
 public:
  explicit Bits(const std::vector<std::uint8_t>& b) : b_(b) {}

  bool bit(std::uint32_t* v) {
    if (pos_ >= b_.size() * 8)
      return false;
    *v = (b_[pos_ / 8] >> (7 - pos_ % 8)) & 1u;
    ++pos_;
    return true;
  }
  bool bits(int n, std::uint32_t* v) {
    std::uint32_t r = 0;
    for (int i = 0; i < n; ++i) {
      std::uint32_t x = 0;
      if (!bit(&x))
        return false;
      r = (r << 1) | x;
    }
    *v = r;
    return true;
  }
  bool skip(std::size_t n) {
    if (n > b_.size() * 8 - pos_)
      return false;
    pos_ += n;
    return true;
  }
  // ue(v), up to 31 leading zeros.
  bool ue(std::uint32_t* v) {
    int zeros = 0;
    std::uint32_t x = 0;
    for (;;) {
      if (!bit(&x))
        return false;
      if (x != 0)
        break;
      if (++zeros > 31)
        return false;
    }
    std::uint32_t rest = 0;
    if (!bits(zeros, &rest))
      return false;
    *v = static_cast<std::uint32_t>((std::uint64_t{1} << zeros) - 1 + rest);
    return true;
  }
  bool se() {
    std::uint32_t v = 0;
    return ue(&v);
  }

 private:
  const std::vector<std::uint8_t>& b_;
  std::size_t pos_ = 0;
};

// The profiles whose SPS carries chroma_format_idc and the scaling matrices.
bool high_profile(std::uint32_t p) {
  switch (p) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
      return true;
    default:
      return false;
  }
}

bool skip_scaling_list(Bits* b, int count) {
  std::int32_t last = 8;
  std::int32_t next = 8;
  for (int j = 0; j < count; ++j) {
    if (next != 0) {
      std::uint32_t code = 0;
      if (!b->ue(&code))
        return false;
      // se(v) from ue(v): 1, -1, 2, -2, ...
      const std::int64_t delta = (code & 1u) != 0
                                     ? static_cast<std::int64_t>(code / 2 + 1)
                                     : -static_cast<std::int64_t>(code / 2);
      next = static_cast<std::int32_t>(((last + delta) % 256 + 256) % 256);
    }
    if (next != 0)
      last = next;
  }
  return true;
}

// seq_parameter_set_data up to the VUI's video_signal_type. *format is set
// once the fields it holds are read (`*have_format`); the return value says
// whether the color was.
bool read_sps(const std::vector<std::uint8_t>& rbsp,
              H264Color* out,
              H264Format* format,
              bool* have_format) {
  *have_format = false;
  Bits b(rbsp);
  std::uint32_t profile = 0;
  std::uint32_t constraints = 0;
  std::uint32_t v = 0;
  if (!b.bits(8, &profile) || !b.bits(8, &constraints) || !b.skip(8) ||
      !b.ue(&v))  // level, id
    return false;
  H264Format f;
  f.profile_idc = static_cast<std::uint8_t>(profile);
  f.constraint_flags = static_cast<std::uint8_t>(constraints);
  if (high_profile(profile)) {
    std::uint32_t chroma = 0;
    if (!b.ue(&chroma) || chroma > 3)
      return false;
    if (chroma == 3 && !b.skip(1))  // separate_colour_plane_flag
      return false;
    std::uint32_t luma_minus8 = 0;
    std::uint32_t chroma_minus8 = 0;
    // 0..6 in every profile (7.4.2.1.1).
    if (!b.ue(&luma_minus8) || !b.ue(&chroma_minus8) || luma_minus8 > 6 ||
        chroma_minus8 > 6)
      return false;
    f.chroma_format_idc = static_cast<std::uint8_t>(chroma);
    f.bit_depth_luma = static_cast<std::uint8_t>(luma_minus8 + 8);
    f.bit_depth_chroma = static_cast<std::uint8_t>(chroma_minus8 + 8);
  }
  *format = f;
  *have_format = true;
  if (high_profile(profile)) {
    std::uint32_t scaling = 0;
    if (!b.skip(1) || !b.bit(&scaling))  // qpprime_y_zero_transform_bypass
      return false;
    if (scaling != 0) {
      const int lists = f.chroma_format_idc != 3 ? 8 : 12;
      for (int i = 0; i < lists; ++i) {
        std::uint32_t present = 0;
        if (!b.bit(&present))
          return false;
        if (present != 0 && !skip_scaling_list(&b, i < 6 ? 16 : 64))
          return false;
      }
    }
  }
  std::uint32_t poc_type = 0;
  if (!b.ue(&v) || !b.ue(&poc_type))  // log2_max_frame_num_minus4
    return false;
  if (poc_type == 0) {
    if (!b.ue(&v))
      return false;
  } else if (poc_type == 1) {
    std::uint32_t cycle = 0;
    if (!b.skip(1) || !b.se() || !b.se() || !b.ue(&cycle) || cycle > 255)
      return false;
    for (std::uint32_t i = 0; i < cycle; ++i)
      if (!b.se())
        return false;
  } else if (poc_type != 2) {
    return false;
  }
  std::uint32_t frame_mbs_only = 0;
  if (!b.ue(&v) || !b.skip(1) || !b.ue(&v) || !b.ue(&v) ||
      !b.bit(&frame_mbs_only))
    return false;
  if (frame_mbs_only == 0 && !b.skip(1))  // mb_adaptive_frame_field_flag
    return false;
  std::uint32_t cropping = 0;
  if (!b.skip(1) || !b.bit(&cropping))  // direct_8x8_inference_flag
    return false;
  if (cropping != 0)
    for (int i = 0; i < 4; ++i)
      if (!b.ue(&v))
        return false;
  std::uint32_t flag = 0;
  if (!b.bit(&flag) || flag == 0)  // vui_parameters_present_flag
    return false;
  if (!b.bit(&flag))  // aspect_ratio_info_present_flag
    return false;
  if (flag != 0) {
    std::uint32_t idc = 0;
    if (!b.bits(8, &idc) || (idc == 255 && !b.skip(32)))  // Extended_SAR
      return false;
  }
  if (!b.bit(&flag) || (flag != 0 && !b.skip(1)))  // overscan
    return false;
  if (!b.bit(&flag) || flag == 0)  // video_signal_type_present_flag
    return false;
  std::uint32_t full = 0;
  std::uint32_t described = 0;
  if (!b.skip(3) || !b.bit(&full) || !b.bit(&described))  // video_format
    return false;
  std::uint32_t matrix = 2;
  if (described != 0 && (!b.skip(16) || !b.bits(8, &matrix)))
    return false;
  out->matrix = static_cast<std::uint8_t>(matrix);
  out->full_range = full != 0;
  return true;
}

}  // namespace

namespace {

// The first SPS ahead of the first coded slice for which `take` says it has
// what it wants, with the emulation prevention bytes taken out.
template <typename Take>
bool find_sps(const std::uint8_t* annexb, std::size_t size, Take take) {
  if (annexb == nullptr)
    return false;
  std::size_t i = 0;
  while (i + 3 < size) {
    // The next start code; the NAL begins after it.
    if (annexb[i] != 0 || annexb[i + 1] != 0 || annexb[i + 2] != 1) {
      ++i;
      continue;
    }
    i += 3;
    const std::uint8_t type = annexb[i] & 0x1f;
    if (type == 1 || type == 5)  // a coded slice: nothing ahead of it said
      return false;
    if (type != 7) {
      ++i;
      continue;
    }
    // The SPS's RBSP: up to the next start code, with the emulation
    // prevention bytes taken out.
    std::vector<std::uint8_t> rbsp;
    int zeros = 0;
    for (std::size_t j = i + 1; j < size && rbsp.size() < kMaxSps; ++j) {
      const std::uint8_t c = annexb[j];
      if (zeros >= 2 && c <= 1)
        break;  // 00 00 00 or 00 00 01: the next start code
      if (zeros >= 2 && c == 3) {
        zeros = 0;
        continue;
      }
      zeros = c == 0 ? zeros + 1 : 0;
      rbsp.push_back(c);
    }
    if (take(rbsp))
      return true;
    ++i;
  }
  return false;
}

}  // namespace

bool h264_color(const std::uint8_t* annexb, std::size_t size, H264Color* out) {
  if (out == nullptr)
    return false;
  return find_sps(annexb, size, [&](const std::vector<std::uint8_t>& rbsp) {
    H264Color c;
    H264Format f;
    bool have_format = false;
    if (!read_sps(rbsp, &c, &f, &have_format))
      return false;
    *out = c;
    return true;
  });
}

bool h264_format(const std::uint8_t* annexb,
                 std::size_t size,
                 H264Format* out) {
  if (out == nullptr)
    return false;
  return find_sps(annexb, size, [&](const std::vector<std::uint8_t>& rbsp) {
    H264Color c;
    H264Format f;
    bool have_format = false;
    (void)read_sps(rbsp, &c, &f, &have_format);
    if (have_format)
      *out = f;
    return have_format;
  });
}

}  // namespace acl::media

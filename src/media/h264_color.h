// SPDX-License-Identifier: Apache-2.0
// Provenance: ITU-T H.264 clauses 7.3.2.1.1 (seq_parameter_set_data) and
// E.1.1 (vui_parameters). No GPL-3.0 sources.
//
// What an H.264 sequence parameter set says of its picture: its format (the
// profile, chroma format and bit depth), and its color, the matrix and the
// range from the VUI's video_signal_type. For an engine whose
// decoder does not pass them on: GStreamer 1.28's h264parse gives the matrix
// but never the range, and 1.16's gives neither.

#ifndef ACL_MEDIA_H264_COLOR_H
#define ACL_MEDIA_H264_COLOR_H

#include <cstddef>
#include <cstdint>

namespace acl::media {

struct H264Color {
  std::uint8_t matrix = 2;  // H.273 matrix coefficients; 2 = not said
  bool full_range = false;
};

// The picture format an H.264 SPS says: the profile, and the chroma format
// and bit depths, which only the High profiles carry (4:2:0, 8 bits for the
// others).
struct H264Format {
  std::uint8_t profile_idc = 0;
  std::uint8_t constraint_flags = 0;  // constraint_set0_flag in the top bit
  std::uint8_t chroma_format_idc = 1;
  std::uint8_t bit_depth_luma = 8;
  std::uint8_t bit_depth_chroma = 8;
  bool deep() const { return bit_depth_luma > 8 || bit_depth_chroma > 8; }
};

// Looks through the Annex B bytes for a sequence parameter set ahead of the
// first coded slice and reads its color. True with *out set when one is found
// and says it (video_signal_type_present_flag); false otherwise, with *out
// untouched. The bytes come from the stream: every read is bounds-checked.
bool h264_color(const std::uint8_t* annexb, std::size_t size, H264Color* out);

// The same search for the SPS's format. True with *out set when an SPS ahead
// of the first coded slice reads as far as its bit depths.
bool h264_format(const std::uint8_t* annexb, std::size_t size, H264Format* out);

}  // namespace acl::media

#endif  // ACL_MEDIA_H264_COLOR_H

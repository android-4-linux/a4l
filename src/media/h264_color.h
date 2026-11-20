// SPDX-License-Identifier: Apache-2.0
// Provenance: ITU-T H.264 clauses 7.3.2.1.1 (seq_parameter_set_data) and
// E.1.1 (vui_parameters). No GPL-3.0 sources.
//
// What an H.264 sequence parameter set says of its picture's color: the
// matrix and the range from the VUI's video_signal_type. For an engine whose
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

// Looks through the Annex B bytes for a sequence parameter set ahead of the
// first coded slice and reads its color. True with *out set when one is found
// and says it (video_signal_type_present_flag); false otherwise, with *out
// untouched. The bytes come from the stream: every read is bounds-checked.
bool h264_color(const std::uint8_t* annexb, std::size_t size, H264Color* out);

}  // namespace acl::media

#endif  // ACL_MEDIA_H264_COLOR_H

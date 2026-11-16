// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (docs/adr/ADR-0009 Decision 3, C5); DRM
// fourcc and modifier values per drm_fourcc.h; fstatfs(2), lseek(2). No
// GPL-3.0 sources.
//
// What the --split broker checks about a message before it acts on it. The
// app process is hostile by assumption (ADR-0009 Decision 3): every field
// here was written by code the APK supplied, and the next reader is the
// compositor.
//
// The checks are pure functions over the message and a few facts the broker
// gathers itself, so that they can be tested and fuzzed without a compositor.
#ifndef ACL_WM_SPLIT_VALIDATE_H
#define ACL_WM_SPLIT_VALIDATE_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ipc/ipc.h"

namespace acl::wm {

// What the broker found out about a descriptor the child sent, without
// trusting anything the child said about it.
struct BufferFacts {
  bool is_dmabuf = false;  // the kernel says the file is a dma-buf
  std::uint64_t size = 0;  // its size in bytes; 0 if unknown
};

// fstatfs() for the dma-buf filesystem magic and lseek() for the size. Does
// not map or read the buffer.
BufferFacts inspect_buffer(int fd) noexcept;

// What a frame is checked against.
struct FrameLimits {
  bool have_surface = false;
  // What SurfaceCreate asked for and got, or a size the broker told the
  // child to take since.
  std::uint32_t surface_w = 0;
  std::uint32_t surface_h = 0;
  // The modifiers the compositor advertised for the surface's format.
  std::span<const std::uint64_t> modifiers;
  int slots = 64;  // buffer_id is -1 (the presenter picks) or below this
};

enum class FrameVerdict {
  Ok,
  NoSurface,    // no surface has been created
  NoBuffer,     // no descriptor came with the message
  NotDmabuf,    // the descriptor is not a dma-buf
  BadSize,      // width or height is not the surface's
  BadFormat,    // not one of the formats the presenter imports
  BadModifier,  // not linear and not advertised by the compositor
  BadPlanes,    // plane count out of range, or an auxiliary plane out of bounds
  BadLayout,    // stride or offset do not fit the buffer
  BadSlot,      // buffer_id out of range
};
const char* verdict_name(FrameVerdict v) noexcept;
inline constexpr int kFrameVerdictCount = 10;

// The frame as the compositor would be asked to import it. `fd_present` is
// whether a descriptor arrived; `facts` is inspect_buffer() of it.
FrameVerdict check_frame(const ipc::PresentFrame& frame,
                         bool fd_present,
                         const BufferFacts& facts,
                         const FrameLimits& limits) noexcept;

// What a video frame is checked against. It goes on its own subsurface, so
// its size is the decoded picture's and not the surface's.
struct VideoLimits {
  bool have_surface = false;
  // The modifiers the compositor advertised for the frame's own format.
  std::span<const std::uint64_t> modifiers;
};

// True for the formats a video frame may have: what a hardware decoder
// exports. NV12 (8-bit 4:2:0) and P010 (10-bit 4:2:0).
bool video_format_accepted(std::uint32_t drm_format) noexcept;

// A video frame as the compositor would be asked to import it. Stricter on
// the modifier than check_frame(): the pair of format and modifier must have
// been advertised, linear included, because a compositor need not take YUV
// at all.
FrameVerdict check_video_frame(const ipc::VideoFrame& frame,
                               bool fd_present,
                               const BufferFacts& facts,
                               const VideoLimits& limits) noexcept;

// True if the rectangle has an area and lies within kMaxVideoCoord, so that
// the arithmetic done on it cannot overflow.
bool check_video_rect(const ipc::VideoRect& rect) noexcept;

// True if an audio stream of this description may be opened: a stream id in
// range, a rate and a channel count a device takes.
bool check_audio_open(const ipc::AudioOpen& open) noexcept;

// True if `bytes` of samples are a write the broker takes for a stream of
// `channels`: not empty, and a whole number of 16-bit frames.
bool check_audio_write(std::size_t bytes, std::uint32_t channels) noexcept;

// True if a surface of this size may be created.
bool check_surface(const ipc::SurfaceInfo& info) noexcept;

// A string the child supplied, made fit to show: valid UTF-8, no control
// characters, at most `max_bytes` long and never cut inside a character.
// `fallback` if nothing is left.
std::string clean_label(std::string_view in,
                        std::size_t max_bytes,
                        std::string_view fallback);

}  // namespace acl::wm

#endif  // ACL_WM_SPLIT_VALIDATE_H

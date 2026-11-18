// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design. No GPL-3.0 sources.
//
// What CodecDecoder asks of a video decode engine other than its own FFmpeg
// one: take access units, give back frames as dma-bufs, flush.

#ifndef ACL_MEDIA_CODEC_ENGINE_H
#define ACL_MEDIA_CODEC_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "gfx/dmabuf_frame.h"

namespace acl::media {

class CodecEngine {
 public:
  virtual ~CodecEngine() = default;

  // 0 taken, 1 not taken (receive_frame first, then send it again), -1 +
  // *error. data == nullptr: end of stream.
  virtual int send_packet(const std::uint8_t* data,
                          std::size_t size,
                          std::int64_t pts_us,
                          std::string* error) = 0;
  // 1 + *out, 0 none ready, -1 + *error. out->hold keeps the frame's memory
  // from being decoded into again for as long as a copy of the frame lives.
  virtual int receive_frame(gfx::DmabufFrame* out,
                            std::int64_t* pts_us,
                            std::string* error) = 0;
  // Drop what was sent and not yet received. The next packet is a key frame.
  virtual void flush() = 0;
};

}  // namespace acl::media

#endif  // ACL_MEDIA_CODEC_ENGINE_H

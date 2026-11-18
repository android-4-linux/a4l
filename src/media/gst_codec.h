// SPDX-License-Identifier: Apache-2.0
// Provenance: GStreamer public API documentation (appsrc, appsink,
// GstElementFactory, GstVideoMeta, gstdmabuf.h). GStreamer is
// LGPL-2.1-or-later, dynamically linked. No GPL-3.0 sources.
//
// A video decoder that is whatever GStreamer has installed for the codec: the
// highest-ranked decoder element, which on a product is the SoC vendor's
// hardware decoder (OpenMAX, V4L2, VA, ...). The same code runs on every
// architecture; what differs is the plugins on the device. The engine under
// CodecDecoder where FFmpeg has no hardware decoder to offer.

#ifndef ACL_MEDIA_GST_CODEC_H
#define ACL_MEDIA_GST_CODEC_H

#include "acl_config.h"

#if ACL_HAVE_GL && ACL_HAVE_GST

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "gfx/dmabuf_frame.h"

namespace acl::media {

class GstCodec {
 public:
  // An appsrc ! [parser] ! decoder ! appsink pipeline for `mime`, playing.
  // `csd` is the codec-specific data, sent ahead of the first access unit.
  // nullptr + *error when GStreamer does not start or has no decoder for the
  // MIME. A decoder that fails once data flows reports it from
  // send_packet/receive_frame.
  static std::unique_ptr<GstCodec> configure(const std::string& mime,
                                             const std::uint8_t* csd,
                                             std::size_t csd_size,
                                             std::string* error);
  ~GstCodec();
  GstCodec(const GstCodec&) = delete;
  GstCodec& operator=(const GstCodec&) = delete;

  // Same contracts as CodecDecoder's.
  int send_packet(const std::uint8_t* data,
                  std::size_t size,
                  std::int64_t pts_us,
                  std::string* error);
  int receive_frame(gfx::DmabufFrame* out,
                    std::int64_t* pts_us,
                    std::string* error);
  void flush();

 private:
  GstCodec();
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace acl::media

#endif  // ACL_HAVE_GL && ACL_HAVE_GST
#endif  // ACL_MEDIA_GST_CODEC_H

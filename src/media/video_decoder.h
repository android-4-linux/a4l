// SPDX-License-Identifier: Apache-2.0
// Provenance: FFmpeg public API (libavcodec/libavformat/libavutil — LGPL, dynamic link only, per
// plan.md policy; dlopen'd RTLD_LOCAL like src/audio/soundpool.cpp), the VAAPI hwaccel + DRM_PRIME
// export contract (libavutil/hwcontext_vaapi.h, hwcontext_drm.h — av_hwframe_map DIRECT), and the
// Linux dma-buf plane layout. The zero-copy video-decode source under MediaCodec setVideoSurface
// ([[flutter-texture-bridge]] M5): a hardware decoder's output frame reaches acl as a DmabufFrame
// (dma-buf fd + layout), which ahb_import routes to the ImageReader consumer with NO pixel copy. No GPL-3.0 sources.

#ifndef ACL_MEDIA_VIDEO_DECODER_H
#define ACL_MEDIA_VIDEO_DECODER_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <cstdint>
#include <memory>
#include <string>

#include "gfx/dmabuf_frame.h"

namespace acl::media {

// Decodes a video file/stream on the GPU (VAAPI; the same code maps V4L2-M2M/DRM devices) and yields
// each frame as a gfx::DmabufFrame — the decoder's own output surface exported as a DRM_PRIME dma-buf,
// ZERO-COPY (av_hwframe_map DIRECT, not a transfer/download). The caller ahb_import()s the frame
// (which dups the fd) before requesting the next; the fd in `out` is owned by the decoder and valid
// only until the next next_frame()/destruction. FFmpeg is resolved at runtime; if it or a VAAPI
// device is unavailable, open() returns nullptr + *error (callers SKIP, not fail).
class VideoDecoder {
 public:
  // Open `path` and set up hardware decode of its first video stream. nullptr + *error on failure
  // (no FFmpeg / no VAAPI device / no video stream / unsupported codec).
  static std::unique_ptr<VideoDecoder> open(const std::string& path, std::string* error);

  ~VideoDecoder();
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // Decode + map the next frame. Returns 1 and fills *out on success, 0 at end-of-stream, -1 + *error
  // on a decode error. *out.fd is owned by the decoder (valid until the next call) — ahb_import it now.
  int next_frame(gfx::DmabufFrame* out, std::string* error);

  std::uint32_t width() const;
  std::uint32_t height() const;

 private:
  VideoDecoder();
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace acl::media

#endif  // ACL_HAVE_GL
#endif  // ACL_MEDIA_VIDEO_DECODER_H

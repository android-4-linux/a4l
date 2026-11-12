// SPDX-License-Identifier: Apache-2.0
// Provenance: FFmpeg public API (libavcodec/libavformat/libavutil — LGPL,
// dynamic link only, per plan.md policy; dlopen'd RTLD_LOCAL like
// src/audio/soundpool.cpp), the VAAPI hwaccel + DRM_PRIME export contract
// (libavutil/hwcontext_vaapi.h, hwcontext_drm.h — av_hwframe_map DIRECT), and
// the Linux dma-buf plane layout. The zero-copy video-decode source under
// MediaCodec setVideoSurface
// ([[flutter-texture-bridge]] M5): a hardware decoder's output frame reaches
// acl as a DmabufFrame (dma-buf fd + layout), which ahb_import routes to the
// ImageReader consumer with NO pixel copy. No GPL-3.0 sources.

#ifndef ACL_MEDIA_VIDEO_DECODER_H
#define ACL_MEDIA_VIDEO_DECODER_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <cstdint>
#include <memory>
#include <string>

#include "gfx/dmabuf_frame.h"

namespace acl::media {

// Decodes a video file/stream on the GPU (VAAPI; the same code maps
// V4L2-M2M/DRM devices) and yields each frame as a gfx::DmabufFrame — the
// decoder's own output surface exported as a DRM_PRIME dma-buf, ZERO-COPY
// (av_hwframe_map DIRECT, not a transfer/download). The caller ahb_import()s
// the frame (which dups the fd) before requesting the next; the fd in `out` is
// owned by the decoder and valid only until the next next_frame()/destruction.
// FFmpeg is resolved at runtime; if it or a VAAPI device is unavailable, open()
// returns nullptr + *error (callers SKIP, not fail).
class VideoDecoder {
 public:
  // Open `path` and set up hardware decode of its first video stream. nullptr +
  // *error on failure (no FFmpeg / no VAAPI device / no video stream /
  // unsupported codec).
  static std::unique_ptr<VideoDecoder> open(const std::string& path,
                                            std::string* error);

  ~VideoDecoder();
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // Decode + map the next frame. Returns 1 and fills *out on success, 0 at
  // end-of-stream, -1 + *error on a decode error. *out.fd is owned by the
  // decoder (valid until the next call) — ahb_import it now.
  int next_frame(gfx::DmabufFrame* out, std::string* error);

  std::uint32_t width() const;
  std::uint32_t height() const;

 private:
  VideoDecoder();
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// Demuxes a container into encoded access units + codec config —
// android.media.MediaExtractor's role. The counterpart to CodecDecoder
// (MediaCodec): Demuxer.read_sample() -> CodecDecoder.send_packet().
class Demuxer {
 public:
  static std::unique_ptr<Demuxer> open(const std::string& path,
                                       std::string* error);
  ~Demuxer();
  Demuxer(const Demuxer&) = delete;
  Demuxer& operator=(const Demuxer&) = delete;

  const std::string& mime()
      const;  // MediaFormat MIME of the video track ("video/avc", …)
  const std::uint8_t* extradata()
      const;  // codec-specific data (SPS/PPS) — MediaFormat csd
  std::size_t extradata_size() const;
  int width() const;
  int height() const;

  // Next video access unit: 1 and fills *data/*size/*pts_us, 0 at
  // end-of-stream, -1 + *error. The data is valid until the next read_sample()
  // call.
  int read_sample(const std::uint8_t** data,
                  std::size_t* size,
                  std::int64_t* pts_us,
                  std::string* error);

 private:
  Demuxer();
  struct DImpl;
  std::unique_ptr<DImpl> d_;
};

// Decodes encoded access units to zero-copy DRM_PRIME frames — the engine under
// android.media .MediaCodec (setVideoSurface). Configured from a MIME +
// codec-specific data (MediaFormat csd), fed raw access units
// (queueInputBuffer), and yields each decoded frame as a gfx::DmabufFrame the
// caller ahb_import's into the output Surface's ImageReader (queue_imported).
// Same HW backend + zero-copy output as VideoDecoder; the difference is the
// input is pre-demuxed packets, not a file.
class CodecDecoder {
 public:
  // Configure a hardware decoder for `mime` (e.g. "video/avc") with optional
  // codec-specific data (H.264/HEVC SPS+PPS as Annex-B — MediaFormat
  // csd-0[+csd-1] concatenated) and the frame size. nullptr + *error if FFmpeg
  // / a HW device / the codec is unavailable.
  static std::unique_ptr<CodecDecoder> configure(const std::string& mime,
                                                 const std::uint8_t* csd,
                                                 std::size_t csd_size,
                                                 int width,
                                                 int height,
                                                 std::string* error);
  ~CodecDecoder();
  CodecDecoder(const CodecDecoder&) = delete;
  CodecDecoder& operator=(const CodecDecoder&) = delete;

  // Feed one encoded access unit (queueInputBuffer). data=nullptr/size=0
  // signals end-of-stream (drain). Returns 0 sent, 1 EAGAIN (the decoder's
  // output is full — receive_frame first, then retry), -1 err.
  int send_packet(const std::uint8_t* data,
                  std::size_t size,
                  std::int64_t pts_us,
                  std::string* error);

  // Pull the next decoded frame as a zero-copy DRM_PRIME dma-buf. 1 + *out
  // (ahb_import it now — its fd is owned by the decoder until the next call), 0
  // if none is ready yet (feed more input), -1 + *error.
  int receive_frame(gfx::DmabufFrame* out,
                    std::int64_t* pts_us,
                    std::string* error);

 private:
  CodecDecoder();
  struct CImpl;
  std::unique_ptr<CImpl> d_;
};

}  // namespace acl::media

#endif  // ACL_HAVE_GL
#endif  // ACL_MEDIA_VIDEO_DECODER_H

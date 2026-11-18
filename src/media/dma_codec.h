// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design over dmabuf-codec's public decoder interface
// (src/dma_decoder.h, src/dma_decoder_factory.h; MIT). The V4L2 capability
// and format ioctls are the Linux UAPI (linux/videodev2.h). No GPL-3.0
// sources.
//
// A hardware video decoder reached with no media framework in between:
// dmabuf-codec's engines (V4L2 stateful and stateless, OpenMAX IL, VA-API),
// whichever the device has. Frames are the decoder's own buffers, as dma-bufs,
// with no copy.
//
// A decoder may keep a frame's planes in one buffer or in a buffer each; the
// frame says which (gfx::DmabufFrame::plane_fd). One caution, measured on a
// Pi 5: its HEVC decoder's default is a buffer each in Broadcom's column
// format, and Mesa's V3D samples that with the luma's column height for both
// planes, so chroma is wrong past the first 128-pixel column. The one-buffer
// form of that format samples correctly, so configure() asks for one buffer
// from a decoder that offers both.

#ifndef ACL_MEDIA_DMA_CODEC_H
#define ACL_MEDIA_DMA_CODEC_H

#include "acl_config.h"

#if ACL_HAVE_DMABUF_CODEC

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "media/codec_engine.h"

namespace v4l2wc {
class IDmaDecoder;
}

namespace acl::media {

class DmaCodec final : public CodecEngine {
 public:
  // Makes a decoder; called again when the stream changes geometry and when a
  // flush finds frames still in use. nullptr when none can be made.
  using Factory = std::function<std::unique_ptr<v4l2wc::IDmaDecoder>()>;

  // A decoder for `mime` from the engines the device has. `csd` (Annex-B
  // parameter sets) goes ahead of the first access unit and of the first
  // after a flush. nullptr + *error when the MIME has no engine here.
  static std::unique_ptr<DmaCodec> configure(const std::string& mime,
                                             const std::uint8_t* csd,
                                             std::size_t csd_size,
                                             int width,
                                             int height,
                                             std::string* error);
  // The same over decoders from `factory`, for tests.
  static std::unique_ptr<DmaCodec> with_factory(Factory factory,
                                                const std::uint8_t* csd,
                                                std::size_t csd_size,
                                                std::string* error);
  ~DmaCodec() override;
  DmaCodec(const DmaCodec&) = delete;
  DmaCodec& operator=(const DmaCodec&) = delete;

  int send_packet(const std::uint8_t* data,
                  std::size_t size,
                  std::int64_t pts_us,
                  std::string* error) override;
  int receive_frame(gfx::DmabufFrame* out,
                    std::int64_t* pts_us,
                    std::string* error) override;
  void flush() override;

 private:
  DmaCodec();
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace acl::media

#endif  // ACL_HAVE_DMABUF_CODEC
#endif  // ACL_MEDIA_DMA_CODEC_H

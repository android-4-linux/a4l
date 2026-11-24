// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl implementation. Public APIs only: Khronos EGL/GLES2
// (EGL_EXT_image_dma_buf_import, OES_EGL_image_external), gbm. No GPL-3.0
// sources.
//
// A decoded video frame as a picture the compositor takes.
//
// A hardware decoder exports NV12 or P010. A SurfaceView's video goes to the
// video plane as it comes, and the compositor does the YUV to RGB conversion
// on scanout or in its renderer. A compositor that does not advertise the
// decoder's format takes none of that, and the app plays with no picture.
// VideoConverter draws the frame into an XBGR8888 LINEAR dma-buf, a format a
// compositor that takes any dma-buf lists: one GPU pass, no CPU copy of the
// picture.
#ifndef ACL_GFX_VIDEO_CONVERT_H
#define ACL_GFX_VIDEO_CONVERT_H

#include <cstdint>
#include <memory>
#include <string>

#include "gfx/dmabuf_frame.h"

namespace acl::gfx {

class VideoConverter {
 public:
  // A converter on its own gbm GL device. nullptr with `error` set where
  // there is none (no GL in this build, no render node).
  static std::unique_ptr<VideoConverter> Create(std::string* error);
  ~VideoConverter();
  VideoConverter(const VideoConverter&) = delete;
  VideoConverter& operator=(const VideoConverter&) = delete;

  // Draw `in` into a buffer and fill `*out` with it: the picture's size, or
  // `width` x `height` when both are given (the picture scaled to it),
  // DRM_FORMAT_XBGR8888, LINEAR. The buffer has unused rows below the
  // picture. `out->fd` belongs to the converter and
  // `out->hold` keeps the buffer from being drawn into again: whoever keeps
  // the frame keeps the hold, as for a decoder's surface. False, with
  // `error`, when the frame cannot be imported or every buffer is still
  // held; the caller drops the frame. Any thread, one at a time.
  bool convert(const DmabufFrame& in,
               DmabufFrame* out,
               std::string* error,
               std::uint32_t width = 0,
               std::uint32_t height = 0);

  // Make `count` of the buffers for output size width x height now, so that
  // the first frames do not wait for them. False, with `error`, if one
  // cannot be made. Any thread, one at a time with convert().
  bool prepare(std::uint32_t width,
               std::uint32_t height,
               int count,
               std::string* error);

  // Convert a linear NV12 picture of picture_width x picture_height `times`
  // at output size width x height, and throw the results away: a driver that
  // builds its shaders and copies at the first draws (Mesa's v3d) does so
  // now.
  bool rehearse(std::uint32_t width,
                std::uint32_t height,
                std::uint32_t picture_width,
                std::uint32_t picture_height,
                int times,
                std::string* error);

  // Buffers the converter keeps for one picture size. More than the video
  // plane holds on screen and in flight (wm: 8 under --split).
  static constexpr int kPoolSize = 12;

  struct State;

 private:
  explicit VideoConverter(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

}  // namespace acl::gfx

#endif  // ACL_GFX_VIDEO_CONVERT_H

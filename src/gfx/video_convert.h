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

  // Draw `in` into a buffer of the same size and fill `*out` with it:
  // DRM_FORMAT_XBGR8888, LINEAR. `out->fd` belongs to the converter and
  // `out->hold` keeps the buffer from being drawn into again: whoever keeps
  // the frame keeps the hold, as for a decoder's surface. False, with
  // `error`, when the frame cannot be imported or every buffer is still
  // held; the caller drops the frame. Any thread, one at a time.
  bool convert(const DmabufFrame& in, DmabufFrame* out, std::string* error);

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

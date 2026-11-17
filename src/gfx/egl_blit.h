// SPDX-License-Identifier: Apache-2.0
// Provenance: Khronos EGL/GLES2 public API (EGL_EXT_image_dma_buf_import,
// EGL_EXT_image_dma_buf_import_modifiers, GL_OES_EGL_image_external); the
// GlDevice/GlWindow seam in gl_egl.h. Original a4l design. No GPL-3.0 sources.
//
// Present a rendered dma-buf frame through an EGL window on a4l's toplevel.
//
// The GPU present path normally hands its dma-bufs to the compositor over
// linux-dmabuf. Some vendor stacks give a window's buffers properties that
// only their own EGL can allocate and pass on (hardware compression, with
// metadata linux-dmabuf does not carry). There the frame has to reach the
// screen through an EGL window: each dma-buf is imported as an EGLImage,
// drawn to the window as one textured quad, and swapped. The EGL
// implementation owns the window's buffers and commits the wl_surface.
//
// Compiled only with the host GL device (ACL_HAVE_GL).

#ifndef ACL_GFX_EGL_BLIT_H
#define ACL_GFX_EGL_BLIT_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <cstdint>
#include <memory>
#include <string>

#include "gfx/dmabuf_frame.h"

namespace acl::gfx {

class EglBlit {
 public:
  // How many distinct source buffers are kept imported, by slot.
  static constexpr int kSlots = 4;

  virtual ~EglBlit() = default;
  EglBlit(const EglBlit&) = delete;
  EglBlit& operator=(const EglBlit&) = delete;

  // An EGL window of `width` x `height` on `wl_surface`, with what is needed
  // to draw a dma-buf to it. Null with *error where the EGL implementation
  // cannot import dma-bufs or sample them. Every later call must come from
  // the thread that calls this.
  static std::unique_ptr<EglBlit> Create(void* wl_display,
                                         void* wl_surface,
                                         int width,
                                         int height,
                                         std::string* error);

  // Draw `frame` to the window and swap. `slot` names which of the producer's
  // buffers it is, so that its import is kept from one frame to the next; a
  // slot whose dma-buf has changed is imported again. Returns once the GPU
  // has read the frame: the producer may render into that buffer again. The
  // context current on the calling thread before the call is current again
  // after it.
  virtual bool present(const DmabufFrame& frame,
                       int slot,
                       std::string* error) = 0;

 protected:
  EglBlit() = default;
};

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
#endif  // ACL_GFX_EGL_BLIT_H

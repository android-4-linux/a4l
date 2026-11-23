// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation. Public APIs only: Khronos EGL/GLES2
// (EGL_EXT_image_dma_buf_import, OES_EGL_image_external), Android SDK docs
// (SurfaceTexture.updateTexImage). No GPL-3.0 sources.
//
// A SurfaceTexture's frame in the app's own GL. An app that samples a
// SurfaceTexture (updateTexImage, then a samplerExternalOES) expects the
// latest frame bound to its GL_TEXTURE_EXTERNAL_OES texture in the context
// current on its thread. graphics_jni holds the frame as a dma-buf and calls
// these through hooks a4l gives it (acl_graphics_set_st_gl_hooks).
#ifndef ACL_GFX_SURFACE_TEXTURE_GL_H
#define ACL_GFX_SURFACE_TEXTURE_GL_H

#include <cstdint>

#include "gfx/dmabuf_frame.h"

// A decoder's frame as it is, for a SurfaceTexture: every plane, uncropped,
// with the transform an app's texture coordinates need. The layout
// graphics_jni takes it in (tools/aosp/graphics_jni/st_frame.h; the two must
// match). fd[i] is plane i's dma-buf.
struct AclStYuvFrame {
  std::uint32_t width, height;   // the picture
  std::uint32_t crop_x, crop_y;  // where it starts in the buffer
  std::uint32_t drm_format;
  std::uint64_t modifier;
  std::uint32_t plane_count;
  int fd[4];
  std::uint32_t offset[4], stride[4];
  std::uint8_t yuv_matrix;  // H.273 matrix coefficients: video_color()'s
  std::uint8_t yuv_full_range;
  float transform[16];  // SurfaceTexture.getTransformMatrix, column-major
};

namespace acl::gfx {

// The frame described for a SurfaceTexture, with its transform. The fds are
// the frame's own, not copies.
AclStYuvFrame st_yuv_frame(const DmabufFrame& frame);

// SurfaceTexture.getTransformMatrix for `frame` bound whole to an external
// texture, top row first: texture coordinate (0, 0), the bottom left in GL,
// maps to the picture's bottom left and (1, 1) to its top right. The image
// ends where the picture does on the right and at the bottom; a picture that
// starts inside the buffer on the left or at the top is moved in by a further
// texel there, so linear filtering does not reach the rows and columns
// outside it.
void st_transform(const DmabufFrame& frame, float out[16]);

// Bind one plane of a dma-buf (`drm_format`, `modifier`) to the external
// texture `tex` of the GL context current on the calling thread, through an
// EGLImage on that context's display. Returns a handle that keeps the image,
// for st_gl_unbind once the texture has been bound to another, or null when
// no context is current or the import fails.
void* st_gl_bind(int tex,
                 int fd,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::uint32_t stride,
                 std::uint32_t offset,
                 std::uint32_t drm_format,
                 std::uint64_t modifier);

// st_gl_bind for a decoder's frame as it is (st_yuv_frame).
void* st_gl_bind_yuv(int tex, const AclStYuvFrame* frame);

// Destroy a handle st_gl_bind returned. Any thread.
void st_gl_unbind(void* handle);

}  // namespace acl::gfx

#endif  // ACL_GFX_SURFACE_TEXTURE_GL_H

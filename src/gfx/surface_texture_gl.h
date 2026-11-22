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

namespace acl::gfx {

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

// Destroy a handle st_gl_bind returned. Any thread.
void st_gl_unbind(void* handle);

}  // namespace acl::gfx

#endif  // ACL_GFX_SURFACE_TEXTURE_GL_H

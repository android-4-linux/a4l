// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation. Public APIs only: Khronos EGL/GLES2
// (EGL_EXT_image_dma_buf_import, OES_EGL_image_external). No GPL-3.0 sources.

#include "gfx/surface_texture_gl.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "gfx/ahardware_buffer.h"
#include "gfx/dmabuf_frame.h"

namespace acl::gfx {
namespace {

using DestroyImageFn = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using ImageTargetFn = void (*)(GLenum, GLeglImageOES);

struct Bound {
  EGLDisplay dpy;
  EGLImageKHR image;
  DestroyImageFn destroy;
};

}  // namespace

void* st_gl_bind(int tex,
                 int fd,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::uint32_t stride,
                 std::uint32_t offset,
                 std::uint32_t drm_format,
                 std::uint64_t modifier) {
  const EGLDisplay dpy = eglGetCurrentDisplay();
  if (dpy == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT ||
      tex <= 0 || fd < 0)
    return nullptr;
  static const auto create = reinterpret_cast<EglCreateImageFn>(
      eglGetProcAddress("eglCreateImageKHR"));
  static const auto destroy =
      reinterpret_cast<DestroyImageFn>(eglGetProcAddress("eglDestroyImageKHR"));
  static const auto target = reinterpret_cast<ImageTargetFn>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (create == nullptr || destroy == nullptr || target == nullptr)
    return nullptr;
  DmabufFrame f{};
  f.fd = fd;
  f.width = width;
  f.height = height;
  f.planes[0].stride = stride;
  f.planes[0].offset = offset;
  f.drm_format = drm_format;
  f.modifier = modifier;
  AHardwareBuffer* ahb = ahb_import(f);  // its own copy of the descriptor
  if (ahb == nullptr)
    return nullptr;
  const auto image =
      static_cast<EGLImageKHR>(egl_image_from_ahb(dpy, ahb, nullptr, create));
  ahb_release(ahb);  // the image holds the memory
  if (image == EGL_NO_IMAGE_KHR)
    return nullptr;
  // The app's binding of GL_TEXTURE_EXTERNAL_OES is left as it was.
  GLint was = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &was);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLuint>(tex));
  target(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
  const bool ok = glGetError() == GL_NO_ERROR;
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLuint>(was));
  if (!ok) {
    destroy(dpy, image);
    return nullptr;
  }
  return new Bound{dpy, image, destroy};
}

void st_gl_unbind(void* handle) {
  auto* b = static_cast<Bound*>(handle);
  if (b == nullptr)
    return;
  b->destroy(b->dpy, b->image);
  delete b;
}

}  // namespace acl::gfx

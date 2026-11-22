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

namespace {

// Bind `f` to the external texture `tex` of the current context.
void* bind_frame(int tex, const DmabufFrame& f) {
  const EGLDisplay dpy = eglGetCurrentDisplay();
  if (dpy == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT ||
      tex <= 0 || f.fd < 0)
    return nullptr;
  static const auto create = reinterpret_cast<EglCreateImageFn>(
      eglGetProcAddress("eglCreateImageKHR"));
  static const auto destroy =
      reinterpret_cast<DestroyImageFn>(eglGetProcAddress("eglDestroyImageKHR"));
  static const auto target = reinterpret_cast<ImageTargetFn>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (create == nullptr || destroy == nullptr || target == nullptr)
    return nullptr;
  AHardwareBuffer* ahb = ahb_import(f);  // its own copy of the descriptors
  if (ahb == nullptr)
    return nullptr;
  const auto image =
      static_cast<EGLImageKHR>(egl_image_from_ahb(dpy, ahb, nullptr, create));
  ahb_release(ahb);  // the image holds the memory
  if (image == EGL_NO_IMAGE_KHR)
    return nullptr;
  // An error the app left pending would read as this bind's; Android's
  // consumer clears them before binding too.
  for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
  }
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

}  // namespace

void* st_gl_bind(int tex,
                 int fd,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::uint32_t stride,
                 std::uint32_t offset,
                 std::uint32_t drm_format,
                 std::uint64_t modifier) {
  DmabufFrame f{};
  f.fd = fd;
  f.width = width;
  f.height = height;
  f.planes[0].stride = stride;
  f.planes[0].offset = offset;
  f.drm_format = drm_format;
  f.modifier = modifier;
  return bind_frame(tex, f);
}

void* st_gl_bind_yuv(int tex, const AclStYuvFrame* y) {
  if (y == nullptr || y->plane_count == 0 ||
      y->plane_count > static_cast<std::uint32_t>(DmabufFrame::kMaxPlanes))
    return nullptr;
  DmabufFrame f{};
  f.fd = y->fd[0];
  f.width = y->width;
  f.height = y->height;
  f.crop_x = y->crop_x;
  f.crop_y = y->crop_y;
  f.drm_format = y->drm_format;
  f.modifier = y->modifier;
  f.plane_count = y->plane_count;
  for (std::uint32_t i = 0; i < y->plane_count; ++i) {
    f.planes[i].fd = y->fd[i];
    f.planes[i].offset = y->offset[i];
    f.planes[i].stride = y->stride[i];
  }
  f.yuv_matrix = y->yuv_matrix;
  f.yuv_full_range = y->yuv_full_range != 0;
  return bind_frame(tex, f);
}

AclStYuvFrame st_yuv_frame(const DmabufFrame& frame) {
  AclStYuvFrame y{};
  y.width = frame.width;
  y.height = frame.height;
  y.crop_x = frame.crop_x;
  y.crop_y = frame.crop_y;
  y.drm_format = frame.drm_format;
  y.modifier = frame.modifier;
  y.plane_count = frame.plane_count;
  for (int i = 0; i < DmabufFrame::kMaxPlanes; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    y.fd[i] = u < frame.plane_count ? frame.plane_fd(u) : -1;
    y.offset[i] = frame.planes[i].offset;
    y.stride[i] = frame.planes[i].stride;
  }
  y.yuv_matrix = frame.yuv_matrix;
  y.yuv_full_range = frame.yuv_full_range ? 1 : 0;
  st_transform(frame, y.transform);
  return y;
}

void st_transform(const DmabufFrame& frame, float out[16]) {
  const float bw = static_cast<float>(frame.buffer_width());
  const float bh = static_cast<float>(frame.buffer_height());
  const float w = static_cast<float>(frame.width);
  const float h = static_cast<float>(frame.height);
  // A texel off each edge that has buffer beyond it.
  const float left = frame.crop_x > 0 ? 1.0F : 0.0F;
  const float top = frame.crop_y > 0 ? 1.0F : 0.0F;
  for (int i = 0; i < 16; ++i)
    out[i] = 0.0F;
  if (bw <= 0.0F || bh <= 0.0F || w <= left || h <= top) {
    out[0] = out[5] = out[10] = out[15] = 1.0F;
    return;
  }
  // s' = (crop_x + left + s (w - left)) / bw
  // t' = (crop_y + h - t (h - top)) / bh      (t' = 0 is the buffer's top row)
  out[0] = (w - left) / bw;
  out[5] = -(h - top) / bh;
  out[10] = 1.0F;
  out[12] = (static_cast<float>(frame.crop_x) + left) / bw;
  out[13] = (static_cast<float>(frame.crop_y) + h) / bh;
  out[15] = 1.0F;
}

void st_gl_unbind(void* handle) {
  auto* b = static_cast<Bound*>(handle);
  if (b == nullptr)
    return;
  b->destroy(b->dpy, b->image);
  delete b;
}

}  // namespace acl::gfx

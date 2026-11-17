// SPDX-License-Identifier: Apache-2.0
// Provenance: Khronos EGL/GLES2 public API (EGL_EXT_image_dma_buf_import,
// EGL_EXT_image_dma_buf_import_modifiers, EGL_KHR_image_base,
// GL_OES_EGL_image_external); libdrm drm_fourcc.h. Original a4l design. No
// GPL-3.0 sources.

#include "gfx/egl_blit.h"

#if ACL_HAVE_GL

#include <sys/stat.h>

#include <cstring>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

#include "gfx/gl_egl.h"
#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "acl.gfx.eglblit";

using CreateImageFn = EGLImageKHR (*)(EGLDisplay,
                                      EGLContext,
                                      EGLenum,
                                      EGLClientBuffer,
                                      const EGLint*);
using DestroyImageFn = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using ImageTargetFn = void (*)(GLenum, void*);

bool has_token(const char* list, const char* name) {
  if (list == nullptr)
    return false;
  const std::size_t n = std::strlen(name);
  for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n) {
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
      return true;
  }
  return false;
}

// A dma-buf's inode number is its identity: two fds for one buffer agree.
std::uint64_t identity_of(int fd) {
  struct stat st{};
  return ::fstat(fd, &st) == 0 ? static_cast<std::uint64_t>(st.st_ino) : 0;
}

// The frame's rows run top to bottom and GL's texture rows bottom to top, so
// the quad's top edge takes t = 0.
constexpr char kVertex[] =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";
constexpr char kFragment[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    "varying vec2 uv;\n"
    "void main() { gl_FragColor = texture2D(tex, uv); }\n";

GLuint compile(GLenum type, const char* src, std::string* error) {
  const GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = GL_FALSE;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char buf[256] = {};
    glGetShaderInfoLog(sh, sizeof(buf) - 1, nullptr, buf);
    if (error)
      *error = std::string("shader: ") + buf;
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

// Puts back the context that was current when it was made. The window's
// context is made current for the length of one call only: the thread that
// presents may be one an app also uses GL on.
class CurrentScope {
 public:
  CurrentScope()
      : dpy_(eglGetCurrentDisplay()),
        ctx_(eglGetCurrentContext()),
        draw_(eglGetCurrentSurface(EGL_DRAW)),
        read_(eglGetCurrentSurface(EGL_READ)) {}
  ~CurrentScope() {
    if (ctx_ != EGL_NO_CONTEXT && dpy_ != EGL_NO_DISPLAY)
      eglMakeCurrent(dpy_, draw_, read_, ctx_);
    else if (own_ != EGL_NO_DISPLAY)
      eglMakeCurrent(own_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  void set_own(EGLDisplay d) { own_ = d; }

 private:
  EGLDisplay dpy_;
  EGLContext ctx_;
  EGLSurface draw_, read_;
  EGLDisplay own_ = EGL_NO_DISPLAY;
};

class Impl final : public EglBlit {
 public:
  ~Impl() override {
    if (window_ != nullptr) {
      CurrentScope scope;
      scope.set_own(dpy());
      std::string e;
      if (window_->make_current(&e)) {
        for (Slot& s : slots_)
          drop(s);
        if (program_ != 0)
          glDeleteProgram(program_);
      }
    }
    window_.reset();  // before the device it was made from
    device_.reset();
  }

  bool init(void* wl_display,
            void* wl_surface,
            int w,
            int h,
            std::string* error) {
    device_ = GlDevice::CreateWayland(wl_display, error);
    if (device_ == nullptr)
      return false;
    const char* exts = eglQueryString(dpy(), EGL_EXTENSIONS);
    if (!has_token(exts, "EGL_EXT_image_dma_buf_import")) {
      if (error)
        *error = "EGL has no EGL_EXT_image_dma_buf_import";
      return false;
    }
    modifiers_ = has_token(exts, "EGL_EXT_image_dma_buf_import_modifiers");
    create_image_ =
        reinterpret_cast<CreateImageFn>(eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<DestroyImageFn>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_ = reinterpret_cast<ImageTargetFn>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (create_image_ == nullptr || destroy_image_ == nullptr ||
        image_target_ == nullptr) {
      if (error)
        *error = "EGL has no EGLImage entry points";
      return false;
    }
    window_ = device_->make_window(w, h, wl_surface, {}, error);
    if (window_ == nullptr)
      return false;

    CurrentScope scope;
    scope.set_own(dpy());
    if (!window_->make_current(error))
      return false;
    if (!has_token(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),
                   "GL_OES_EGL_image_external")) {
      if (error)
        *error = "GLES has no GL_OES_EGL_image_external";
      return false;
    }
    // a4l paces frames by the wl_surface's frame callback; a swap that also
    // waited for one would halve the rate.
    eglSwapInterval(dpy(), 0);
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex, error);
    const GLuint fs =
        vs != 0 ? compile(GL_FRAGMENT_SHADER, kFragment, error) : 0;
    if (vs == 0 || fs == 0) {
      if (vs != 0)
        glDeleteShader(vs);
      return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glBindAttribLocation(program_, 0, "pos");
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
      if (error)
        *error = "program link failed";
      return false;
    }
    log::i(kTag, "EGL window present: {}x{} on {} (modifiers {})", w, h,
           device_->renderer(), modifiers_ ? "yes" : "no");
    return true;
  }

  bool present(const DmabufFrame& frame,
               int slot,
               std::string* error) override {
    if (frame.fd < 0 || frame.width == 0 || frame.height == 0 || slot < 0 ||
        slot >= kSlots) {
      if (error)
        *error = "present: no frame";
      return false;
    }
    CurrentScope scope;
    scope.set_own(dpy());
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    if ((window_->width() != w || window_->height() != h) &&
        !window_->resize(w, h, error))
      return false;
    if (!window_->make_current(error))
      return false;

    Slot& s = slots_[slot];
    const std::uint64_t id = identity_of(frame.fd);
    if (s.image != EGL_NO_IMAGE_KHR && (id == 0 || id != s.identity))
      drop(s);
    if (s.image == EGL_NO_IMAGE_KHR && !import(frame, id, &s, error))
      return false;

    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);  // the frame is premultiplied: copy it as it is
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, s.texture);
    glUniform1i(glGetUniformLocation(program_, "tex"), 0);
    static const GLfloat kQuad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    // The producer takes the buffer back when this returns, so the GPU has
    // to have read it by then.
    glFinish();
    return window_->swap(nullptr, error);
  }

 private:
  struct Slot {
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    std::uint64_t identity = 0;
  };

  EGLDisplay dpy() const {
    return static_cast<EGLDisplay>(device_->egl_display());
  }

  void drop(Slot& s) {
    if (s.texture != 0)
      glDeleteTextures(1, &s.texture);
    if (s.image != EGL_NO_IMAGE_KHR)
      destroy_image_(dpy(), s.image);
    s = Slot{};
  }

  bool import(const DmabufFrame& f,
              std::uint64_t id,
              Slot* s,
              std::string* error) {
    if (f.plane_count != 1) {
      if (error)
        *error = "import: only single-plane frames";
      return false;
    }
    std::vector<EGLint> a = {
        EGL_WIDTH,
        static_cast<EGLint>(f.width),
        EGL_HEIGHT,
        static_cast<EGLint>(f.height),
        EGL_LINUX_DRM_FOURCC_EXT,
        static_cast<EGLint>(f.drm_format),
        EGL_DMA_BUF_PLANE0_FD_EXT,
        f.fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        static_cast<EGLint>(f.planes[0].offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<EGLint>(f.planes[0].stride),
    };
    // Without the modifiers extension only a LINEAR buffer can be described.
    if (modifiers_) {
      a.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
      a.push_back(static_cast<EGLint>(f.modifier & 0xffffffffu));
      a.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
      a.push_back(static_cast<EGLint>(f.modifier >> 32));
    } else if (f.modifier != DRM_FORMAT_MOD_LINEAR) {
      if (error)
        *error = "import: a tiled frame and no EGL modifier support";
      return false;
    }
    a.push_back(EGL_NONE);
    const EGLImageKHR img = create_image_(
        dpy(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a.data());
    if (img == EGL_NO_IMAGE_KHR) {
      if (error)
        *error = "eglCreateImageKHR(dma-buf) failed";
      return false;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
    image_target_(GL_TEXTURE_EXTERNAL_OES, img);
    if (glGetError() != GL_NO_ERROR) {
      glDeleteTextures(1, &tex);
      destroy_image_(dpy(), img);
      if (error)
        *error = "glEGLImageTargetTexture2DOES failed";
      return false;
    }
    s->image = img;
    s->texture = tex;
    s->identity = id;
    return true;
  }

  std::unique_ptr<GlDevice> device_;
  std::unique_ptr<GlWindow> window_;
  bool modifiers_ = false;
  CreateImageFn create_image_ = nullptr;
  DestroyImageFn destroy_image_ = nullptr;
  ImageTargetFn image_target_ = nullptr;
  GLuint program_ = 0;
  Slot slots_[kSlots];
};

}  // namespace

std::unique_ptr<EglBlit> EglBlit::Create(void* wl_display,
                                         void* wl_surface,
                                         int width,
                                         int height,
                                         std::string* error) {
  auto impl = std::make_unique<Impl>();
  if (!impl->init(wl_display, wl_surface, width, height, error))
    return nullptr;
  return impl;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

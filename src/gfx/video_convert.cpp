// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl implementation. Public APIs only: Khronos EGL/GLES2
// (EGL_EXT_image_dma_buf_import, OES_EGL_image_external), gbm. No GPL-3.0
// sources.
#include "gfx/video_convert.h"

#include "acl_config.h"

#if ACL_HAVE_GL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "gfx/ahardware_buffer.h"
#include "gfx/gl_egl.h"
#include "gfx/hardware_buffer.h"
#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "acl.gfx.vconv";
constexpr std::uint64_t kModInvalid = 0x00ffffffffffffffull;

// Drawn into a framebuffer object on the buffer: GL's y = -1 is the buffer's
// first row, which is the picture's top, the source's t = 0. The source is
// imported whole; `picture` is the part of it that is the picture: its
// origin and size, as fractions of the source.
constexpr char kVertex[] =
    "attribute vec2 pos;\n"
    "uniform vec4 picture;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  uv = picture.xy + (pos * 0.5 + 0.5) * picture.zw;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";
constexpr char kFragment[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    "varying vec2 uv;\n"
    "void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); }\n";

GLuint compile(GLenum type, const char* src) {
  const GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = GL_FALSE;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

using CreateImageFn = EGLImageKHR (*)(EGLDisplay,
                                      EGLContext,
                                      EGLenum,
                                      EGLClientBuffer,
                                      const EGLint*);
using DestroyImageFn = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
using ImageTargetFn = void (*)(GLenum, GLeglImageOES);

}  // namespace

struct VideoConverter::State {
  // One buffer of the pool. `busy` is shared with the frame's hold, which
  // clears it from whatever thread lets the frame go.
  struct Slot {
    std::shared_ptr<HardwareBuffer> buffer;
    std::shared_ptr<std::atomic<bool>> busy;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0, fbo = 0;
  };

  std::mutex mu;
  std::unique_ptr<GlDevice> device;
  std::unique_ptr<GlWindow> window;  // only so that the context can be current
  std::vector<Slot> pool;
  std::uint32_t pool_w = 0, pool_h = 0;
  GLuint program = 0, texture = 0;
  GLint pos = -1;
  GLint picture = -1;
  CreateImageFn create_image = nullptr;
  DestroyImageFn destroy_image = nullptr;
  ImageTargetFn image_target = nullptr;

  EGLDisplay dpy() const {
    return static_cast<EGLDisplay>(device->egl_display());
  }

  // Leaves the context current on no thread: the next frame may come on
  // another one.
  void release_current() const {
    eglMakeCurrent(dpy(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }

  // Context current. The buffers themselves live on while a frame holds them.
  void drop_pool() {
    for (Slot& slot : pool) {
      if (slot.fbo != 0)
        glDeleteFramebuffers(1, &slot.fbo);
      if (slot.texture != 0)
        glDeleteTextures(1, &slot.texture);
      if (slot.image != EGL_NO_IMAGE_KHR)
        destroy_image(dpy(), slot.image);
    }
    pool.clear();
  }

  // A free buffer of w x h to draw into, made on first use. Context current.
  Slot* take_slot(std::uint32_t w, std::uint32_t h, std::string* error) {
    if (w != pool_w || h != pool_h) {
      drop_pool();
      pool_w = w;
      pool_h = h;
    }
    for (Slot& slot : pool) {
      if (!slot.busy->load(std::memory_order_acquire))
        return &slot;
    }
    if (pool.size() >= static_cast<std::size_t>(kPoolSize)) {
      if (error)
        *error = "every converted buffer is still held";
      return nullptr;
    }
    Slot slot;
    slot.buffer =
        HardwareBuffer::allocate(w, h, HbFormat::R8G8B8X8_UNORM, 0, error);
    if (!slot.buffer)
      return nullptr;
    AHardwareBuffer* ahb = ahb_create(slot.buffer);
    if (ahb != nullptr) {
      slot.image = static_cast<EGLImageKHR>(
          egl_image_from_ahb(dpy(), ahb, nullptr,
                             reinterpret_cast<EglCreateImageFn>(create_image)));
      ahb_release(ahb);
    }
    if (slot.image == EGL_NO_IMAGE_KHR) {
      if (error)
        *error = "the GL driver does not import its own buffer";
      return nullptr;
    }
    glGenTextures(1, &slot.texture);
    glBindTexture(GL_TEXTURE_2D, slot.texture);
    image_target(GL_TEXTURE_2D, slot.image);
    glGenFramebuffers(1, &slot.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           slot.texture, 0);
    const bool complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
      glDeleteFramebuffers(1, &slot.fbo);
      glDeleteTextures(1, &slot.texture);
      destroy_image(dpy(), slot.image);
      if (error)
        *error = "the GL driver does not render to the buffer";
      return nullptr;
    }
    slot.busy = std::make_shared<std::atomic<bool>>(false);
    pool.push_back(std::move(slot));
    return &pool.back();
  }

  bool ensure_program(std::string* error) {
    if (program != 0)
      return true;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (vs == 0 || fs == 0) {
      if (error)
        *error = "shader compile failed (no OES_EGL_image_external?)";
      return false;
    }
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
      glDeleteProgram(program);
      program = 0;
      if (error)
        *error = "shader link failed";
      return false;
    }
    pos = glGetAttribLocation(program, "pos");
    picture = glGetUniformLocation(program, "picture");
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
    return true;
  }
};

VideoConverter::VideoConverter(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

VideoConverter::~VideoConverter() {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mu);
  std::string err;
  if (s.window && s.window->make_current(&err)) {
    s.drop_pool();
    s.release_current();
  }
}

std::unique_ptr<VideoConverter> VideoConverter::Create(std::string* error) {
  auto state = std::make_shared<State>();
  state->device = GlDevice::CreateGbm(error);
  if (!state->device)
    return nullptr;
  // The context needs a surface to be current on; nothing is drawn to it.
  state->window = state->device->make_window(16, 16, nullptr, {}, error);
  if (!state->window)
    return nullptr;
  state->create_image =
      reinterpret_cast<CreateImageFn>(eglGetProcAddress("eglCreateImageKHR"));
  state->destroy_image =
      reinterpret_cast<DestroyImageFn>(eglGetProcAddress("eglDestroyImageKHR"));
  state->image_target = reinterpret_cast<ImageTargetFn>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (state->create_image == nullptr || state->destroy_image == nullptr ||
      state->image_target == nullptr) {
    if (error)
      *error = "no EGL image import on this GL";
    return nullptr;
  }
  // Whatever bring-up left current stays on this thread no longer: convert()
  // may run on another.
  state->release_current();
  log::i(kTag, "video converter on {}", state->device->renderer());
  return std::unique_ptr<VideoConverter>(new VideoConverter(std::move(state)));
}

bool VideoConverter::convert(const DmabufFrame& in,
                             DmabufFrame* out,
                             std::string* error) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mu);
  if (out == nullptr || in.fd < 0 || in.width == 0 || in.height == 0) {
    if (error)
      *error = "no frame";
    return false;
  }
  if (!s.window->make_current(error))
    return false;
  bool ok = false;
  AHardwareBuffer* ahb = nullptr;
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  do {
    if (!s.ensure_program(error))
      break;
    State::Slot* slot = s.take_slot(in.width, in.height, error);
    if (slot == nullptr)
      break;
    ahb = ahb_import(in);
    if (ahb == nullptr) {
      if (error)
        *error = "frame import failed";
      break;
    }
    image = static_cast<EGLImageKHR>(
        egl_image_from_ahb(s.dpy(), ahb, nullptr,
                           reinterpret_cast<EglCreateImageFn>(s.create_image)));
    if (image == EGL_NO_IMAGE_KHR) {
      if (error)
        *error = "the GL driver does not import the decoder's frame";
      break;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, slot->fbo);
    glViewport(0, 0, static_cast<GLsizei>(in.width),
               static_cast<GLsizei>(in.height));
    glDisable(GL_BLEND);
    glUseProgram(s.program);
    // The output is the picture, which starts at (crop_x, crop_y) in the
    // decoder's buffer.
    const auto bw = static_cast<GLfloat>(in.buffer_width());
    const auto bh = static_cast<GLfloat>(in.buffer_height());
    glUniform4f(s.picture, static_cast<GLfloat>(in.crop_x) / bw,
                static_cast<GLfloat>(in.crop_y) / bh,
                static_cast<GLfloat>(in.width) / bw,
                static_cast<GLfloat>(in.height) / bh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, s.texture);
    s.image_target(GL_TEXTURE_EXTERNAL_OES, image);
    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(static_cast<GLuint>(s.pos), 2, GL_FLOAT, GL_FALSE, 0,
                          quad);
    glEnableVertexAttribArray(static_cast<GLuint>(s.pos));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // The compositor reads the buffer with no fence from here, so the draw
    // has to have happened before the frame is handed on.
    glFinish();
    const bool drawn = glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!drawn) {
      if (error)
        *error = "drawing the frame failed";
      break;
    }
    slot->busy->store(true, std::memory_order_release);
    *out = slot->buffer->frame();
    // The buffer was allocated linear (GBM_BO_USE_LINEAR). A driver that
    // reports no modifier for such a buffer still laid it out that way, and
    // the compositor is told the layout, not "unknown".
    if (out->modifier == kModInvalid) {
      out->modifier = 0;
      out->modifier_tiled = false;
    }
    // The buffer is free to draw into again when the last copy of the frame
    // lets go of it; its memory lives as long as any copy does.
    out->hold = std::shared_ptr<void>(
        slot->buffer.get(), [buffer = slot->buffer, busy = slot->busy](void*) {
          busy->store(false, std::memory_order_release);
        });
    ok = true;
  } while (false);
  if (image != EGL_NO_IMAGE_KHR)
    s.destroy_image(s.dpy(), image);
  if (ahb != nullptr)
    ahb_release(ahb);
  s.release_current();
  return ok;
}

}  // namespace acl::gfx

#else  // !ACL_HAVE_GL

#include <utility>

namespace acl::gfx {
struct VideoConverter::State {};
VideoConverter::VideoConverter(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
VideoConverter::~VideoConverter() = default;
std::unique_ptr<VideoConverter> VideoConverter::Create(std::string* error) {
  if (error)
    *error = "built without GL";
  return nullptr;
}
bool VideoConverter::convert(const DmabufFrame&,
                             DmabufFrame*,
                             std::string* error) {
  if (error)
    *error = "built without GL";
  return false;
}
}  // namespace acl::gfx

#endif

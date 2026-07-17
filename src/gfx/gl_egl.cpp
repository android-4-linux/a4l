// SPDX-License-Identifier: Apache-2.0
// Provenance: Mesa EGL/GLES public API (EGL_KHR_platform_wayland / EGL_KHR_platform_gbm,
// eglGetPlatformDisplayEXT from EGL_EXT_platform_base), wayland-egl (wl_egl_window_create/
// resize/destroy), gbm public API (gbm_create_device, gbm_surface_*, gbm_bo_*), Khronos
// EGL/GLES2 headers, libdrm drm_fourcc.h. The wl_egl_window→EGLSurface and the
// gbm→EGLSurface→gbm_bo→dma-buf shapes are the standard Mesa idioms (Weston/kmscube, MIT).
// No GPL-3.0 sources.
//
// GlDevice/GlWindow are the abstract seam (gl_egl.h); this file holds the two self-contained
// backend implementations. A new backend is a new pair of classes here + a branch in
// GlDevice::Create — nothing else in the tree changes.

#include "gfx/gl_egl.h"

#if ACL_HAVE_GL

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gbm.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>

#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "acl.gfx.gl";

// The gbm-backend buffer format. XRGB8888 (opaque) matches the presenter's default and the
// compositor's advertised XRGB8888 modifiers; a fullscreen GL app is opaque.
constexpr std::uint32_t kGbmFormat = GBM_FORMAT_XRGB8888;

std::string egl_err_str(EGLint e) {
  switch (e) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    default: {
      char b[24];
      std::snprintf(b, sizeof b, "EGL_0x%x", e);
      return b;
    }
  }
}
std::string with_egl(const std::string& what) { return what + " (" + egl_err_str(eglGetError()) + ")"; }

EGLDisplay platform_display(EGLenum platform, void* native, std::string* err) {
  auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  EGLDisplay dpy = getPlatformDisplay != nullptr
                       ? getPlatformDisplay(platform, native, nullptr)
                       : eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(native));
  if (dpy == EGL_NO_DISPLAY && err) *err = "eglGetPlatformDisplay returned EGL_NO_DISPLAY";
  return dpy;
}

// Choose a GLES window config + create a shared context on an already-initialized display.
// Shared by both backends. Returns "" on success (filling *ctx/*cfg/*renderer), else the error.
std::string bring_up_context(EGLDisplay dpy, GlBackend backend, EGLContext* ctx_out,
                             EGLConfig* cfg_out, std::string* renderer_out) {
  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) return with_egl("eglBindAPI(ES) failed");

  // gbm requires ALPHA_SIZE 0 (opaque XRGB) and a native-visual match; wayland allows an
  // alpha channel (a subsurface may blend) and matches by attributes only.
  const bool gbm = backend == GlBackend::Gbm;
  const EGLint cfg_attr[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, gbm ? 0 : 8,
      EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8,
      EGL_NONE};
  EGLConfig configs[64];
  EGLint n = 0;
  if (eglChooseConfig(dpy, cfg_attr, configs, 64, &n) != EGL_TRUE || n == 0)
    return with_egl("eglChooseConfig found no GLES2 window config");
  EGLConfig cfg = configs[0];
  if (gbm) {  // pick the config whose native visual id is our gbm/DRM fourcc
    for (EGLint i = 0; i < n; ++i) {
      EGLint vid = 0;
      if (eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &vid) == EGL_TRUE &&
          static_cast<std::uint32_t>(vid) == kGbmFormat) {
        cfg = configs[i];
        break;
      }
    }
  }

  // Prefer a GLES3 context (libGDX GL30; backward-compatible with GLES2), fall back to 2.
  EGLContext ctx = EGL_NO_CONTEXT;
  for (EGLint ver : {3, 2}) {
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, ver, EGL_NONE};
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx != EGL_NO_CONTEXT) break;
  }
  if (ctx == EGL_NO_CONTEXT) return with_egl("eglCreateContext failed");

  // GL_RENDERER needs a current context; EGL_KHR_surfaceless_context lets us do it surfaceless.
  if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE) {
    const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (renderer_out) *renderer_out = (r != nullptr) ? r : "(unknown)";
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  *ctx_out = ctx;
  *cfg_out = cfg;
  return "";
}
}  // namespace

// ── GlWindow base ────────────────────────────────────────────────────────────
bool GlWindow::make_current(std::string* error) {
  if (eglMakeCurrent(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(egl_),
                     static_cast<EGLSurface>(egl_), static_cast<EGLContext>(ctx_)) != EGL_TRUE) {
    if (error) *error = with_egl("eglMakeCurrent(window) failed");
    return false;
  }
  return true;
}

// ── Wayland backend ───────────────────────────────────────────────────────────
namespace {
class WaylandGlWindow final : public GlWindow {
 public:
  WaylandGlWindow(void* dpy, void* ctx, void* cfg)
      : GlWindow(GlBackend::Wayland, dpy, ctx, cfg) {}
  ~WaylandGlWindow() override {
    if (egl_ != nullptr) eglDestroySurface(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(egl_));
    if (egl_win_ != nullptr) wl_egl_window_destroy(egl_win_);
  }

  bool init(void* wl_surface, int w, int h, void* egl_config, std::string* error) {
    if (wl_surface == nullptr) { if (error) *error = "make_window(wayland): null wl_surface"; return false; }
    wl_surf_ = static_cast<struct wl_surface*>(wl_surface);
    egl_win_ = wl_egl_window_create(wl_surf_, w, h);
    if (egl_win_ == nullptr) { if (error) *error = "wl_egl_window_create failed"; return false; }
    // The EGLSurface must share the app's context config (else eglMakeCurrent EGL_BAD_MATCH).
    EGLConfig cfg = egl_config != nullptr ? static_cast<EGLConfig>(egl_config) : static_cast<EGLConfig>(cfg_);
    EGLSurface s = eglCreateWindowSurface(static_cast<EGLDisplay>(dpy_), cfg,
                                          reinterpret_cast<EGLNativeWindowType>(egl_win_), nullptr);
    if (s == EGL_NO_SURFACE) { if (error) *error = with_egl("eglCreateWindowSurface(wl_egl_window) failed"); return false; }
    egl_ = s;
    width_ = w;
    height_ = h;
    return true;
  }

  bool swap(DmabufFrame* /*out*/, std::string* error) override {
    // Mesa's Wayland WSI attaches/commits/throttles the wl_surface here — this IS the present.
    if (eglSwapBuffers(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(egl_)) != EGL_TRUE) {
      if (error) *error = with_egl("eglSwapBuffers failed");
      return false;
    }
    return true;
  }

  bool resize(int w, int h, std::string* /*error*/) override {
    if (w == width_ && h == height_) return true;
    wl_egl_window_resize(egl_win_, w, h, 0, 0);  // Mesa reallocates on the next swap
    width_ = w;
    height_ = h;
    return true;
  }

 private:
  struct wl_egl_window* egl_win_ = nullptr;
  struct wl_surface* wl_surf_ = nullptr;  // borrowed
};

class WaylandGlDevice final : public GlDevice {
 public:
  WaylandGlDevice() : GlDevice(GlBackend::Wayland) {}
  ~WaylandGlDevice() override {
    if (dpy_ != EGL_NO_DISPLAY && dpy_ != nullptr) {
      eglMakeCurrent(static_cast<EGLDisplay>(dpy_), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (ctx_ != nullptr) eglDestroyContext(static_cast<EGLDisplay>(dpy_), static_cast<EGLContext>(ctx_));
      eglTerminate(static_cast<EGLDisplay>(dpy_));  // wl_display is borrowed — not disconnected
    }
  }

  bool init(void* wl_display, std::string* error) {
    if (wl_display == nullptr) { if (error) *error = "CreateWayland: null wl_display"; return false; }
    std::string e;
    EGLDisplay dpy = platform_display(EGL_PLATFORM_WAYLAND_KHR, wl_display, &e);
    if (dpy == EGL_NO_DISPLAY) { if (error) *error = "wayland: " + e; return false; }
    EGLint major = 0, minor = 0;
    if (eglInitialize(dpy, &major, &minor) != EGL_TRUE) { if (error) *error = with_egl("eglInitialize(wayland) failed"); return false; }
    dpy_ = dpy;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig cfg = nullptr;
    if (std::string ce = bring_up_context(dpy, GlBackend::Wayland, &ctx, &cfg, &renderer_); !ce.empty()) {
      if (error) *error = ce;
      return false;
    }
    ctx_ = ctx;
    cfg_ = cfg;
    log::i(kTag, "host GL device: {} (Wayland platform, EGL {}.{})", renderer_, major, minor);
    return true;
  }

  std::unique_ptr<GlWindow> make_window(int width, int height, void* native_surface,
                                        const std::vector<std::uint64_t>& /*modifiers*/,
                                        std::string* error, void* egl_config) override {
    auto win = std::make_unique<WaylandGlWindow>(dpy_, ctx_, cfg_);
    if (!win->init(native_surface, width, height, egl_config, error)) return nullptr;
    return win;
  }
};
}  // namespace

// ── Gbm backend ──────────────────────────────────────────────────────────────
namespace {
class GbmGlWindow final : public GlWindow {
 public:
  GbmGlWindow(void* dpy, void* ctx, void* cfg, gbm_device* gbm)
      : GlWindow(GlBackend::Gbm, dpy, ctx, cfg), gbm_(gbm) {}
  ~GbmGlWindow() override {
    release_front();
    if (egl_ != nullptr) eglDestroySurface(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(egl_));
    if (surf_ != nullptr) gbm_surface_destroy(surf_);
  }

  bool init(int w, int h, const std::vector<std::uint64_t>& modifiers, std::string* error) {
    if (!make_gbm_surface(w, h, modifiers)) { if (error) *error = "gbm_surface_create failed"; return false; }
    EGLSurface s = eglCreateWindowSurface(static_cast<EGLDisplay>(dpy_), static_cast<EGLConfig>(cfg_),
                                          reinterpret_cast<EGLNativeWindowType>(surf_), nullptr);
    if (s == EGL_NO_SURFACE) { if (error) *error = with_egl("eglCreateWindowSurface(gbm) failed"); return false; }
    egl_ = s;
    width_ = w;
    height_ = h;
    return true;
  }

  bool swap(DmabufFrame* out, std::string* error) override {
    auto set = [&](const std::string& m) { if (error) *error = m; return false; };
    if (eglSwapBuffers(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(egl_)) != EGL_TRUE)
      return set(with_egl("eglSwapBuffers failed"));

    release_front();
    gbm_bo* bo = gbm_surface_lock_front_buffer(surf_);
    if (bo == nullptr) return set("gbm_surface_lock_front_buffer returned null");
    locked_ = bo;
    const int fd = gbm_bo_get_fd(bo);  // a fresh dup we own; closed on next swap/destroy
    if (fd < 0) return set("gbm_bo_get_fd failed");
    exported_fd_ = fd;

    if (out != nullptr) {
      *out = DmabufFrame{};
      out->fd = fd;
      out->width = gbm_bo_get_width(bo);
      out->height = gbm_bo_get_height(bo);
      out->drm_format = gbm_bo_get_format(bo);
      out->modifier = gbm_bo_get_modifier(bo);
      const int planes = gbm_bo_get_plane_count(bo);
      out->plane_count = planes > 0 ? static_cast<std::uint32_t>(planes) : 1u;
      if (out->plane_count > DmabufFrame::kMaxPlanes) out->plane_count = DmabufFrame::kMaxPlanes;
      std::uint64_t total = 0;
      for (std::uint32_t i = 0; i < out->plane_count; ++i) {
        out->planes[i].stride = gbm_bo_get_stride_for_plane(bo, static_cast<int>(i));
        out->planes[i].offset = gbm_bo_get_offset(bo, static_cast<int>(i));
        const std::uint64_t end = out->planes[i].offset +
                                  static_cast<std::uint64_t>(out->planes[i].stride) * out->height;
        if (end > total) total = end;
      }
      out->mem_size = total;
      out->modifier_tiled = out->modifier != DRM_FORMAT_MOD_LINEAR &&
                            out->modifier != DRM_FORMAT_MOD_INVALID;
      out->host_mapped = false;
    }
    return true;
  }

  bool resize(int w, int h, std::string* error) override {
    if (w == width_ && h == height_) return true;
    release_front();
    if (egl_ != nullptr) { eglDestroySurface(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(egl_)); egl_ = nullptr; }
    if (surf_ != nullptr) { gbm_surface_destroy(surf_); surf_ = nullptr; }
    if (!make_gbm_surface(w, h, {})) { if (error) *error = "gbm_surface_create (resize) failed"; return false; }
    EGLSurface s = eglCreateWindowSurface(static_cast<EGLDisplay>(dpy_), static_cast<EGLConfig>(cfg_),
                                          reinterpret_cast<EGLNativeWindowType>(surf_), nullptr);
    if (s == EGL_NO_SURFACE) { if (error) *error = with_egl("eglCreateWindowSurface(gbm resize) failed"); return false; }
    egl_ = s;
    width_ = w;
    height_ = h;
    return true;
  }

 private:
  bool make_gbm_surface(int w, int h, const std::vector<std::uint64_t>& modifiers) {
    const std::uint32_t flags = GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT;
    if (!modifiers.empty()) {
      surf_ = gbm_surface_create_with_modifiers2(gbm_, static_cast<std::uint32_t>(w),
                                                 static_cast<std::uint32_t>(h), kGbmFormat,
                                                 modifiers.data(), modifiers.size(), flags);
    }
    if (surf_ == nullptr) {  // no modifiers given, or none supported by the driver
      surf_ = gbm_surface_create(gbm_, static_cast<std::uint32_t>(w),
                                 static_cast<std::uint32_t>(h), kGbmFormat, flags);
    }
    return surf_ != nullptr;
  }
  void release_front() {
    if (exported_fd_ >= 0) { ::close(exported_fd_); exported_fd_ = -1; }
    if (locked_ != nullptr && surf_ != nullptr) { gbm_surface_release_buffer(surf_, locked_); locked_ = nullptr; }
  }

  gbm_device* gbm_ = nullptr;  // borrowed from the device
  gbm_surface* surf_ = nullptr;
  gbm_bo* locked_ = nullptr;   // front buffer currently held for the presenter
  int exported_fd_ = -1;       // dup'd fd handed out in the last DmabufFrame (we own it)
};

class GbmGlDevice final : public GlDevice {
 public:
  GbmGlDevice() : GlDevice(GlBackend::Gbm) {}
  ~GbmGlDevice() override {
    if (dpy_ != EGL_NO_DISPLAY && dpy_ != nullptr) {
      eglMakeCurrent(static_cast<EGLDisplay>(dpy_), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (ctx_ != nullptr) eglDestroyContext(static_cast<EGLDisplay>(dpy_), static_cast<EGLContext>(ctx_));
      eglTerminate(static_cast<EGLDisplay>(dpy_));
    }
    if (gbm_ != nullptr) gbm_device_destroy(gbm_);
    if (drm_fd_ >= 0) ::close(drm_fd_);
  }

  bool init(std::string* error) {
    const char* node = std::getenv("ACL_GL_RENDER_NODE");
    if (node == nullptr || *node == '\0') node = "/dev/dri/renderD128";
    drm_fd_ = ::open(node, O_RDWR | O_CLOEXEC);
    if (drm_fd_ < 0) { if (error) *error = std::string("open ") + node + " failed: " + std::strerror(errno); return false; }
    gbm_ = gbm_create_device(drm_fd_);
    if (gbm_ == nullptr) { if (error) *error = "gbm_create_device failed on " + std::string(node); return false; }

    std::string e;
    EGLDisplay dpy = platform_display(EGL_PLATFORM_GBM_KHR, gbm_, &e);
    if (dpy == EGL_NO_DISPLAY) { if (error) *error = "gbm: " + e; return false; }
    EGLint major = 0, minor = 0;
    if (eglInitialize(dpy, &major, &minor) != EGL_TRUE) { if (error) *error = with_egl("eglInitialize(gbm) failed"); return false; }
    dpy_ = dpy;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig cfg = nullptr;
    if (std::string ce = bring_up_context(dpy, GlBackend::Gbm, &ctx, &cfg, &renderer_); !ce.empty()) {
      if (error) *error = ce;
      return false;
    }
    ctx_ = ctx;
    cfg_ = cfg;
    log::i(kTag, "host GL device: {} (gbm platform, EGL {}.{}, node {})", renderer_, major, minor, node);
    return true;
  }

  std::unique_ptr<GlWindow> make_window(int width, int height, void* /*native_surface*/,
                                        const std::vector<std::uint64_t>& modifiers,
                                        std::string* error, void* /*egl_config*/) override {
    // gbm needs its own visual-matched config (the gbm format); the app config doesn't apply.
    auto win = std::make_unique<GbmGlWindow>(dpy_, ctx_, cfg_, gbm_);
    if (!win->init(width, height, modifiers, error)) return nullptr;
    return win;
  }

 private:
  int drm_fd_ = -1;
  gbm_device* gbm_ = nullptr;
};
}  // namespace

// ── Factories (the single backend-selection point) ──────────────────────────
std::unique_ptr<GlDevice> GlDevice::CreateWayland(void* wl_display, std::string* error) {
  auto dev = std::make_unique<WaylandGlDevice>();
  if (!dev->init(wl_display, error)) return nullptr;
  return dev;
}

std::unique_ptr<GlDevice> GlDevice::CreateGbm(std::string* error) {
  auto dev = std::make_unique<GbmGlDevice>();
  if (!dev->init(error)) return nullptr;
  return dev;
}

std::unique_ptr<GlDevice> GlDevice::Create(void* wl_display, std::string* error) {
  // ACL_GL_BACKEND overrides the wl_display-based default (e.g. force gbm/KMS under a
  // compositor). Unknown values fall through to the default policy.
  const char* forced = std::getenv("ACL_GL_BACKEND");
  const bool want_gbm = (forced != nullptr && std::strcmp(forced, "gbm") == 0) ||
                        (wl_display == nullptr &&
                         !(forced != nullptr && std::strcmp(forced, "wayland") == 0));
  return want_gbm ? CreateGbm(error) : CreateWayland(wl_display, error);
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

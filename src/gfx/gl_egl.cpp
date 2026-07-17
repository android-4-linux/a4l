// SPDX-License-Identifier: Apache-2.0
// Provenance: Mesa EGL/GLES public API (EGL_KHR_platform_wayland / EGL_KHR_platform_gbm,
// eglGetPlatformDisplayEXT from EGL_EXT_platform_base), wayland-egl (wl_egl_window_create/
// resize/destroy), gbm public API (gbm_create_device, gbm_surface_*, gbm_bo_*), Khronos
// EGL/GLES2 headers, libdrm drm_fourcc.h. The wl_egl_window→EGLSurface and the
// gbm→EGLSurface→gbm_bo→dma-buf shapes are the standard Mesa idioms (Weston/kmscube, MIT).
// No GPL-3.0 sources.

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

// The gbm-backend buffer format. XRGB8888 (opaque) matches the presenter's default and
// the compositor's advertised XRGB8888 modifiers; a fullscreen GL app is opaque.
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
}  // namespace

// ── Device ──────────────────────────────────────────────────────────────────
struct GlDevice::Impl {
  GlBackend backend = GlBackend::Wayland;
  // gbm backend only:
  int drm_fd = -1;
  gbm_device* gbm = nullptr;
  // wayland backend only (borrowed, not owned):
  struct wl_display* wl_dpy = nullptr;
  // both:
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLContext ctx = EGL_NO_CONTEXT;
  EGLConfig cfg = nullptr;

  ~Impl() {
    if (dpy != EGL_NO_DISPLAY) {
      eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
      eglTerminate(dpy);
    }
    if (gbm != nullptr) gbm_device_destroy(gbm);
    if (drm_fd >= 0) ::close(drm_fd);
  }
};

GlDevice::GlDevice(GlBackend backend) : impl_(std::make_unique<Impl>()), backend_(backend) {
  impl_->backend = backend;
}
GlDevice::~GlDevice() = default;
void* GlDevice::egl_display() const { return impl_->dpy; }
void* GlDevice::egl_context() const { return impl_->ctx; }
void* GlDevice::egl_config() const { return impl_->cfg; }

namespace {
// The EGL bring-up shared by both backends: choose a GLES window config, create a shared
// context, and record GL_RENDERER. impl_->dpy must already be a valid, initialized
// EGLDisplay. Returns "" on success, else the error string.
std::string init_egl_context(GlDevice::Impl& d, std::string* renderer_out) {
  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) return with_egl("eglBindAPI(ES) failed");

  // gbm requires ALPHA_SIZE 0 (opaque XRGB) and a native-visual match; wayland allows an
  // alpha channel (a subsurface may blend) and matches by attributes only.
  const bool gbm = d.backend == GlBackend::Gbm;
  const EGLint cfg_attr[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, gbm ? 0 : 8,
      EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8,
      EGL_NONE};
  EGLConfig configs[64];
  EGLint n = 0;
  if (eglChooseConfig(d.dpy, cfg_attr, configs, 64, &n) != EGL_TRUE || n == 0)
    return with_egl("eglChooseConfig found no GLES2 window config");
  d.cfg = configs[0];
  if (gbm) {  // pick the config whose native visual id is our gbm/DRM fourcc
    for (EGLint i = 0; i < n; ++i) {
      EGLint vid = 0;
      if (eglGetConfigAttrib(d.dpy, configs[i], EGL_NATIVE_VISUAL_ID, &vid) == EGL_TRUE &&
          static_cast<std::uint32_t>(vid) == kGbmFormat) {
        d.cfg = configs[i];
        break;
      }
    }
  }

  // Prefer a GLES3 context (libGDX GL30; backward-compatible with GLES2), fall back to 2.
  for (EGLint ver : {3, 2}) {
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, ver, EGL_NONE};
    d.ctx = eglCreateContext(d.dpy, d.cfg, EGL_NO_CONTEXT, ctx_attr);
    if (d.ctx != EGL_NO_CONTEXT) break;
  }
  if (d.ctx == EGL_NO_CONTEXT) return with_egl("eglCreateContext failed");

  // GL_RENDERER needs a current context; EGL_KHR_surfaceless_context lets us do it with
  // no surface.
  if (eglMakeCurrent(d.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, d.ctx) == EGL_TRUE) {
    const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (renderer_out) *renderer_out = (r != nullptr) ? r : "(unknown)";
    eglMakeCurrent(d.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  return "";
}

EGLDisplay platform_display(EGLenum platform, void* native, std::string* err) {
  auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  EGLDisplay dpy = EGL_NO_DISPLAY;
  if (getPlatformDisplay != nullptr) {
    dpy = getPlatformDisplay(platform, native, nullptr);
  } else {
    dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(native));
  }
  if (dpy == EGL_NO_DISPLAY && err) *err = "eglGetPlatformDisplay returned EGL_NO_DISPLAY";
  return dpy;
}
}  // namespace

std::unique_ptr<GlDevice> GlDevice::CreateWayland(void* wl_display, std::string* error) {
  auto set = [&](const std::string& m) { if (error) *error = m; return nullptr; };
  if (wl_display == nullptr) return set("CreateWayland: null wl_display");

  auto dev = std::unique_ptr<GlDevice>(new GlDevice(GlBackend::Wayland));
  Impl& d = *dev->impl_;
  d.wl_dpy = static_cast<struct wl_display*>(wl_display);

  std::string e;
  d.dpy = platform_display(EGL_PLATFORM_WAYLAND_KHR, wl_display, &e);
  if (d.dpy == EGL_NO_DISPLAY) return set("wayland: " + e);
  EGLint major = 0, minor = 0;
  if (eglInitialize(d.dpy, &major, &minor) != EGL_TRUE) return set(with_egl("eglInitialize(wayland) failed"));
  if (std::string ce = init_egl_context(d, &dev->renderer_); !ce.empty()) return set(ce);
  log::i(kTag, "host GL device: {} (Wayland platform, EGL {}.{})", dev->renderer_, major, minor);
  return dev;
}

std::unique_ptr<GlDevice> GlDevice::CreateGbm(std::string* error) {
  auto set = [&](const std::string& m) { if (error) *error = m; return nullptr; };

  const char* node = std::getenv("ACL_GL_RENDER_NODE");
  if (node == nullptr || *node == '\0') node = "/dev/dri/renderD128";

  auto dev = std::unique_ptr<GlDevice>(new GlDevice(GlBackend::Gbm));
  Impl& d = *dev->impl_;

  d.drm_fd = ::open(node, O_RDWR | O_CLOEXEC);
  if (d.drm_fd < 0) return set(std::string("open ") + node + " failed: " + std::strerror(errno));
  d.gbm = gbm_create_device(d.drm_fd);
  if (d.gbm == nullptr) return set("gbm_create_device failed on " + std::string(node));

  std::string e;
  d.dpy = platform_display(EGL_PLATFORM_GBM_KHR, d.gbm, &e);
  if (d.dpy == EGL_NO_DISPLAY) return set("gbm: " + e);
  EGLint major = 0, minor = 0;
  if (eglInitialize(d.dpy, &major, &minor) != EGL_TRUE) return set(with_egl("eglInitialize(gbm) failed"));
  if (std::string ce = init_egl_context(d, &dev->renderer_); !ce.empty()) return set(ce);
  log::i(kTag, "host GL device: {} (gbm platform, EGL {}.{}, node {})", dev->renderer_, major, minor, node);
  return dev;
}

// ── Window ──────────────────────────────────────────────────────────────────
struct GlWindow::Impl {
  GlBackend backend = GlBackend::Wayland;
  // Handles cached from the owning device at make_window time, so the window never
  // touches GlDevice's private state (and can outlive nothing it doesn't own).
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLContext ctx = EGL_NO_CONTEXT;
  EGLConfig cfg = nullptr;
  gbm_device* gbm = nullptr;   // gbm backend only
  EGLSurface egl = EGL_NO_SURFACE;
  // wayland backend:
  struct wl_egl_window* egl_win = nullptr;
  struct wl_surface* wl_surf = nullptr;  // borrowed
  // gbm backend:
  gbm_surface* surf = nullptr;
  gbm_bo* locked = nullptr;   // front buffer currently held for the presenter
  int exported_fd = -1;       // dup'd fd handed out in the last DmabufFrame (we own it)

  void release_front() {  // gbm only
    if (exported_fd >= 0) { ::close(exported_fd); exported_fd = -1; }
    if (locked != nullptr && surf != nullptr) { gbm_surface_release_buffer(surf, locked); locked = nullptr; }
  }
  ~Impl() {
    release_front();
    if (egl != EGL_NO_SURFACE && dpy != EGL_NO_DISPLAY) eglDestroySurface(dpy, egl);
    if (egl_win != nullptr) wl_egl_window_destroy(egl_win);
    if (surf != nullptr) gbm_surface_destroy(surf);
  }
};

GlWindow::GlWindow(GlBackend backend, GlDevice* /*dev*/)
    : impl_(std::make_unique<Impl>()), backend_(backend) {
  impl_->backend = backend;
}
GlWindow::~GlWindow() = default;
void* GlWindow::egl_surface() const { return impl_->egl; }

std::unique_ptr<GlWindow> GlDevice::make_window(int width, int height, void* native_surface,
                                                const std::vector<std::uint64_t>& modifiers,
                                                std::string* error) {
  auto set = [&](const std::string& m) { if (error) *error = m; return nullptr; };
  Impl& d = *impl_;

  auto win = std::unique_ptr<GlWindow>(new GlWindow(backend_, this));
  win->width_ = width;
  win->height_ = height;
  GlWindow::Impl& w = *win->impl_;
  w.dpy = d.dpy;
  w.ctx = d.ctx;
  w.cfg = d.cfg;
  w.gbm = d.gbm;

  if (backend_ == GlBackend::Wayland) {
    if (native_surface == nullptr) return set("make_window(wayland): null wl_surface");
    w.wl_surf = static_cast<struct wl_surface*>(native_surface);
    w.egl_win = wl_egl_window_create(w.wl_surf, width, height);
    if (w.egl_win == nullptr) return set("wl_egl_window_create failed");
    w.egl = eglCreateWindowSurface(d.dpy, d.cfg, reinterpret_cast<EGLNativeWindowType>(w.egl_win), nullptr);
    if (w.egl == EGL_NO_SURFACE) return set(with_egl("eglCreateWindowSurface(wl_egl_window) failed"));
    return win;
  }

  // gbm backend
  const std::uint32_t flags = GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT;
  gbm_surface* s = nullptr;
  if (!modifiers.empty()) {
    s = gbm_surface_create_with_modifiers2(d.gbm, static_cast<std::uint32_t>(width),
                                           static_cast<std::uint32_t>(height), kGbmFormat,
                                           modifiers.data(), modifiers.size(), flags);
  }
  if (s == nullptr) {  // no modifiers given, or none supported by the driver
    s = gbm_surface_create(d.gbm, static_cast<std::uint32_t>(width),
                           static_cast<std::uint32_t>(height), kGbmFormat, flags);
  }
  if (s == nullptr) return set("gbm_surface_create failed");
  w.surf = s;
  w.egl = eglCreateWindowSurface(d.dpy, d.cfg, reinterpret_cast<EGLNativeWindowType>(s), nullptr);
  if (w.egl == EGL_NO_SURFACE) return set(with_egl("eglCreateWindowSurface(gbm) failed"));
  return win;
}

bool GlWindow::make_current(std::string* error) {
  Impl& w = *impl_;
  if (eglMakeCurrent(w.dpy, w.egl, w.egl, w.ctx) != EGL_TRUE) {
    if (error) *error = with_egl("eglMakeCurrent(window) failed");
    return false;
  }
  return true;
}

bool GlWindow::resize(int width, int height, std::string* error) {
  if (width == width_ && height == height_) return true;
  Impl& w = *impl_;
  if (backend_ == GlBackend::Wayland) {
    // wl_egl_window_resize is cheap; Mesa reallocates buffers on the next swap.
    wl_egl_window_resize(w.egl_win, width, height, 0, 0);
    width_ = width;
    height_ = height;
    return true;
  }
  // gbm: tear down the EGLSurface + gbm_surface and recreate at the new size.
  w.release_front();
  if (w.egl != EGL_NO_SURFACE) { eglDestroySurface(w.dpy, w.egl); w.egl = EGL_NO_SURFACE; }
  if (w.surf != nullptr) { gbm_surface_destroy(w.surf); w.surf = nullptr; }
  gbm_surface* s = gbm_surface_create(w.gbm, static_cast<std::uint32_t>(width),
                                      static_cast<std::uint32_t>(height), kGbmFormat,
                                      GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
  if (s == nullptr) { if (error) *error = "gbm_surface_create (resize) failed"; return false; }
  w.surf = s;
  w.egl = eglCreateWindowSurface(w.dpy, w.cfg, reinterpret_cast<EGLNativeWindowType>(s), nullptr);
  if (w.egl == EGL_NO_SURFACE) { if (error) *error = with_egl("eglCreateWindowSurface(gbm resize) failed"); return false; }
  width_ = width;
  height_ = height;
  return true;
}

bool GlWindow::swap(DmabufFrame* out, std::string* error) {
  Impl& w = *impl_;
  auto set = [&](const std::string& m) { if (error) *error = m; return false; };

  if (eglSwapBuffers(w.dpy, w.egl) != EGL_TRUE) return set(with_egl("eglSwapBuffers failed"));

  if (backend_ == GlBackend::Wayland) return true;  // Mesa committed the (sub)surface

  // gbm: lock the newly-swapped front buffer and describe its dma-buf for acl's presenter.
  impl_->release_front();
  gbm_bo* bo = gbm_surface_lock_front_buffer(impl_->surf);
  if (bo == nullptr) return set("gbm_surface_lock_front_buffer returned null");
  impl_->locked = bo;

  const int fd = gbm_bo_get_fd(bo);  // a fresh dup we own; closed on next swap/destroy
  if (fd < 0) return set("gbm_bo_get_fd failed");
  impl_->exported_fd = fd;

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

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

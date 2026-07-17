// SPDX-License-Identifier: Apache-2.0
// Provenance: Mesa EGL/GLES public API (EGL_KHR_platform_wayland, EGL_KHR_platform_gbm,
// EGL_EXT_platform_base's eglGetPlatformDisplayEXT), wayland-egl (wl_egl_window_*), gbm
// public API (gbm.h), Khronos EGL/GLES2 headers, libdrm drm_fourcc.h. ADR-4 (zero-copy
// present, hwui dropped), plan.md §2/A02 (SurfaceView → wl_subsurface). No GPL-3.0 sources.
//
// The host GL device (GLSurfaceView path): a headless GPU OpenGL ES context, the native
// underside of an app's GLThread — com.google.android.gles_jni.EGLImpl and
// android.opengl.GLES20 run on THIS context (host Mesa), and EGLImpl's
// eglCreateWindowSurface targets a window owned here.
//
// GlDevice / GlWindow are ABSTRACT — the pluggable seam. Each backend is a self-contained
// implementation (in gl_egl.cpp) selected once by GlDevice::Create; every layer above (the
// EGLImpl/GLES20 JNI, the present path) uses the seam and never branches on backend, and a
// new backend is a new class that touches none of the others:
//
//   Wayland (default, compositor path): eglGetPlatformDisplay(WAYLAND, acl's wl_display);
//     a window is a wl_egl_window on the SurfaceView's wl_(sub)surface, and swap() lets
//     Mesa's Wayland WSI allocate buffers (linux-dmabuf + modifiers) and present/composite
//     it — SurfaceView → wl_subsurface falls out for free.
//   Gbm (no-compositor / automotive KMS): eglGetPlatformDisplay(GBM, a DRM render node);
//     swap() exports the front gbm_bo as a dma-buf for acl's own presenter, uniform with
//     the Vulkan path.
//
// Compiled only when the host offers EGL + GLESv2 + gbm + wayland-egl (ACL_HAVE_GL).

#ifndef ACL_GFX_GL_EGL_H
#define ACL_GFX_GL_EGL_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gfx/dmabuf_frame.h"  // gfx::DmabufFrame (the gbm-path present interchange)

namespace acl::gfx {

enum class GlBackend { Wayland, Gbm };

// A backend's GLES render window: the seam the JNI/present layers drive. Concrete
// subclasses (in gl_egl.cpp) own the backend-specific surface (wl_egl_window / gbm_surface);
// the common EGL handles + make_current live on the base. Owned by the caller; must not
// outlive its GlDevice.
class GlWindow {
 public:
  virtual ~GlWindow() = default;
  GlWindow(const GlWindow&) = delete;
  GlWindow& operator=(const GlWindow&) = delete;

  int width() const { return width_; }
  int height() const { return height_; }
  GlBackend backend() const { return backend_; }
  bool exports_dmabuf() const { return backend_ == GlBackend::Gbm; }
  void* egl_surface() const { return egl_; }  // EGLSurface — EGLImpl's window-surface result

  // Make this window's EGLSurface + context current on the calling thread (common to all
  // backends).
  bool make_current(std::string* error);

  // Present the just-rendered GLES frame. Wayland: Mesa commits the (sub)surface (this IS
  // the present) and *out is left empty. Gbm: the swapped front gbm_bo is locked and *out
  // filled with its dma-buf (fd owned by this window, valid until the next swap/destruction)
  // for acl's presenter. `out` may be null when the caller doesn't need the export.
  virtual bool swap(DmabufFrame* out, std::string* error) = 0;

  // Resize (SurfaceHolder.surfaceChanged). Wayland resizes the wl_egl_window; gbm recreates
  // the gbm_surface. Cheap no-op when unchanged.
  virtual bool resize(int width, int height, std::string* error) = 0;

 protected:
  GlWindow(GlBackend backend, void* egl_display, void* egl_context, void* egl_config)
      : backend_(backend), dpy_(egl_display), ctx_(egl_context), cfg_(egl_config) {}
  GlBackend backend_;
  void* dpy_ = nullptr;   // EGLDisplay  (opaque in the header)
  void* ctx_ = nullptr;   // EGLContext
  void* cfg_ = nullptr;   // EGLConfig
  void* egl_ = nullptr;   // EGLSurface  (set by the subclass at creation)
  int width_ = 0;
  int height_ = 0;
};

// The process-wide host GL device: an EGL display + shared GLES context on one platform.
// ABSTRACT — GlDevice::Create picks the concrete backend. Create* return nullptr with *error
// when the host lacks the pieces (callers SKIP, they don't fail). The device outlives the
// windows it makes; both share its context.
class GlDevice {
 public:
  virtual ~GlDevice() = default;
  GlDevice(const GlDevice&) = delete;
  GlDevice& operator=(const GlDevice&) = delete;

  // The single backend-selection entry — the ONLY place the backend is chosen, so every
  // layer above stays backend-agnostic through the GlWindow seam. A non-null `wl_display`
  // selects the Wayland platform (compositor path); null selects gbm (no-compositor/KMS).
  // ACL_GL_BACKEND=wayland|gbm forces one (gbm even under a compositor — to exercise KMS).
  static std::unique_ptr<GlDevice> Create(void* wl_display, std::string* error);

  // The concrete backends, if a caller wants one explicitly (Create fronts these).
  static std::unique_ptr<GlDevice> CreateWayland(void* wl_display, std::string* error);
  static std::unique_ptr<GlDevice> CreateGbm(std::string* error);

  GlBackend backend() const { return backend_; }
  const std::string& renderer() const { return renderer_; }  // GL_RENDERER string

  // EGL handles the JNI EGLImpl layer binds onto — its EGL10 display/context are these, so
  // an app GLThread's eglMakeCurrent lands on this device.
  void* egl_display() const { return dpy_; }
  void* egl_context() const { return ctx_; }
  void* egl_config() const { return cfg_; }

  // Create a WxH GLES render window sharing this device's context.
  //   Wayland: `native_surface` is a struct wl_surface* to render into (the SurfaceView's
  //     subsurface, or the toplevel for a fullscreen GL app); `modifiers` unused.
  //   Gbm: `native_surface` ignored; `modifiers` is the compositor's advertised DRM set so
  //     the gbm_bo is allocated scanout-tiled.
  virtual std::unique_ptr<GlWindow> make_window(int width, int height, void* native_surface,
                                                const std::vector<std::uint64_t>& modifiers,
                                                std::string* error) = 0;

 protected:
  explicit GlDevice(GlBackend backend) : backend_(backend) {}
  GlBackend backend_;
  std::string renderer_;
  void* dpy_ = nullptr;   // EGLDisplay  (set by the subclass' bring-up)
  void* ctx_ = nullptr;   // EGLContext
  void* cfg_ = nullptr;   // EGLConfig
};

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
#endif  // ACL_GFX_GL_EGL_H

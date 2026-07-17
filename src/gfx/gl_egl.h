// SPDX-License-Identifier: Apache-2.0
// Provenance: Mesa EGL/GLES public API (EGL_KHR_platform_wayland, EGL_KHR_platform_gbm,
// EGL_EXT_platform_base's eglGetPlatformDisplayEXT), wayland-egl (wl_egl_window_*), gbm
// public API (gbm.h), Khronos EGL/GLES2 headers, libdrm drm_fourcc.h. ADR-4 (zero-copy
// present, hwui dropped), plan.md §2/A02 (SurfaceView → wl_subsurface). No GPL-3.0 sources.
//
// The host GL device (GLSurfaceView path): a headless GPU OpenGL ES context, the native
// underside of an app's GLThread — com.google.android.gles_jni.EGLImpl and
// android.opengl.GLES20 run on THIS context (host Mesa), and EGLImpl's
// eglCreateWindowSurface targets a window owned here. Two interchangeable backends behind
// one seam (the JNI/GLES layers don't care which is under a GlWindow):
//
//   Wayland (default, compositor path): eglGetPlatformDisplay(WAYLAND, acl's wl_display);
//     a GlWindow is a wl_egl_window on the SurfaceView's wl_(sub)surface, and eglSwapBuffers
//     lets Mesa's Wayland WSI allocate buffers (linux-dmabuf + modifiers) and present/
//     composite it — SurfaceView → wl_subsurface falls out for free.
//   Gbm (no-compositor / automotive KMS): eglGetPlatformDisplay(GBM, a DRM render node);
//     a GlWindow is a gbm_surface, and swap() exports the front gbm_bo as a dma-buf for
//     acl's own presenter (drm-cxx KMS-direct scanout), uniform with the Vulkan path.
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

class GlWindow;

// The process-wide host GL device. The Create* factories return nullptr with *error when
// the host lacks the pieces (callers SKIP, they don't fail). The device outlives the
// windows it makes; both share its GLES context.
class GlDevice {
 public:
  ~GlDevice();
  GlDevice(const GlDevice&) = delete;
  GlDevice& operator=(const GlDevice&) = delete;

  // Wayland-platform device on acl's existing wl_display (borrowed; Mesa's WSI presents).
  // The default compositor path. `wl_display` is a struct wl_display* (opaque here).
  static std::unique_ptr<GlDevice> CreateWayland(void* wl_display, std::string* error);

  // Gbm-platform device on a DRM render node (default /dev/dri/renderD128, override via
  // ACL_GL_RENDER_NODE). acl presents the exported dma-buf; for the no-compositor path.
  static std::unique_ptr<GlDevice> CreateGbm(std::string* error);

  GlBackend backend() const { return backend_; }
  const std::string& renderer() const { return renderer_; }  // GL_RENDERER string

  // EGL handles the JNI EGLImpl layer (Stage 2) binds onto — its EGL10 display/context
  // are these, so an app GLThread's eglMakeCurrent lands on this device.
  void* egl_display() const;   // EGLDisplay
  void* egl_context() const;   // shared EGLContext (GLES)
  void* egl_config() const;    // the chosen EGLConfig

  // Create a WxH GLES render window sharing this device's context.
  //   Wayland backend: `native_surface` is a struct wl_surface* to render into (the
  //     SurfaceView's subsurface, or the toplevel for a fullscreen GL app). `modifiers`
  //     unused (Mesa negotiates them via linux-dmabuf feedback). nullptr surface fails.
  //   Gbm backend: `native_surface` ignored; `modifiers` is the compositor's advertised
  //     DRM modifier set (from feedback) so the gbm_bo is allocated scanout-tiled.
  // nullptr + *error on failure.
  std::unique_ptr<GlWindow> make_window(int width, int height, void* native_surface,
                                        const std::vector<std::uint64_t>& modifiers,
                                        std::string* error);

  struct Impl;

 private:
  explicit GlDevice(GlBackend backend);
  std::unique_ptr<Impl> impl_;
  GlBackend backend_;
  std::string renderer_;
};

class GlWindow {
 public:
  ~GlWindow();
  GlWindow(const GlWindow&) = delete;
  GlWindow& operator=(const GlWindow&) = delete;

  int width() const { return width_; }
  int height() const { return height_; }
  GlBackend backend() const { return backend_; }
  bool exports_dmabuf() const { return backend_ == GlBackend::Gbm; }

  // Make this window's EGLSurface + the device context current on the calling thread.
  bool make_current(std::string* error);

  // Present the just-rendered GLES frame (eglSwapBuffers). On the Wayland backend Mesa
  // commits the (sub)surface and `out` is left empty — this IS the present. On the gbm
  // backend the swapped front gbm_bo is locked and *out filled with its dma-buf (fd owned
  // by this window, valid until the next swap/destruction) for acl's presenter. `out` may
  // be null when the caller doesn't need the export (always fine on Wayland).
  bool swap(DmabufFrame* out, std::string* error);

  // Resize the window (SurfaceHolder surfaceChanged). Wayland resizes the wl_egl_window;
  // gbm recreates the gbm_surface. Cheap no-op when unchanged.
  bool resize(int width, int height, std::string* error);

  void* egl_surface() const;   // EGLSurface — EGLImpl's eglCreateWindowSurface result

  struct Impl;

 private:
  friend class GlDevice;
  GlWindow(GlBackend backend, GlDevice* dev);
  std::unique_ptr<Impl> impl_;
  GlBackend backend_;
  int width_ = 0;
  int height_ = 0;
};

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
#endif  // ACL_GFX_GL_EGL_H

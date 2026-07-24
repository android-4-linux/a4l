// SPDX-License-Identifier: Apache-2.0
// Provenance: Khronos EGL 1.0 spec + EGL/egl.h; the JNI contract of AOSP's
// com.google.android.gles_jni.EGLImpl (frameworks/base/opengl/java + core/jni, behavioral
// — method set, EGL*Impl wrapper-object handle model). Re-authored, not copied. No GPL-3.0 sources.
//
// GLSurfaceView Stage 2a: acl's com.google.android.gles_jni.EGLImpl natives — the EGL10 the
// app's GLThread drives. Almost all methods pass through to host Mesa libEGL on the EGLDisplay
// acl hands out; only two need acl: _eglGetDisplay (acl's wl_display-bound display) and
// _eglCreateWindowSurface (android.view.Surface -> a wl_egl_window on acl's wl_surface). The
// runtime supplies both via AclEgl10Hooks so this stays free of gfx::GlDevice / Surface details.
// Compiled when ACL_HAVE_GL (needs libEGL) and JNI headers are present.

#ifndef ACL_GFX_EGL10_JNI_H
#define ACL_GFX_EGL10_JNI_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <jni.h>
#include <EGL/egl.h>

namespace acl::gfx {

// The two runtime-provided seams the EGL10 natives can't answer on their own.
struct AclEgl10Hooks {
  // eglGetDisplay(EGL_DEFAULT_DISPLAY): the EGLDisplay the app should use — acl's
  // wl_display-bound display (so a later wl_egl_window shares that connection), NOT Mesa's
  // default. Returns EGL_NO_DISPLAY if the GL device isn't up.
  EGLDisplay (*get_display)(void* user) = nullptr;
  // _eglCreateWindowSurface(display, config, android.view.Surface, attribs): resolve the
  // Surface to acl's wl_surface, make a wl_egl_window + EGLSurface. EGL_NO_SURFACE on failure.
  // `attribs` is an EGL_NONE-terminated list or nullptr.
  EGLSurface (*create_window_surface)(JNIEnv* env, jobject surface, EGLConfig config,
                                      const EGLint* attribs, void* user) = nullptr;
  // _eglCreateWindowSurfaceTexture(display, config, android.graphics.SurfaceTexture, attribs): the
  // producer end of a TextureView's SurfaceTexture. Back it with an OFFSCREEN, dma-buf-exporting GL
  // window (gbm), so the app's GLThread renders + swaps into a buffer acl owns — instead of a
  // wl_surface (a TextureView is composited into the view tree, not a subsurface). The produced
  // frame is routed to the SurfaceTexture consumer for the TextureLayer to draw. EGL_NO_SURFACE on
  // failure. `attribs` is an EGL_NONE-terminated list or nullptr.
  EGLSurface (*create_surfacetexture_surface)(JNIEnv* env, jobject surface_texture, EGLConfig config,
                                              const EGLint* attribs, void* user) = nullptr;
  void* user = nullptr;
};

// Register the EGLImpl natives on com.google.android.gles_jni.EGLImpl, storing `hooks`.
// Returns JNI_OK on success. Called from the runtime at VM boot once the GL device exists.
jint register_egl10(JNIEnv* env, const AclEgl10Hooks& hooks);

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
#endif  // ACL_GFX_EGL10_JNI_H

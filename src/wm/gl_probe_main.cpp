// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl tooling; Khronos GLES2 (glClearColor/glClear/glViewport),
// Mesa EGL_PLATFORM_WAYLAND idiom. Validates the GLSurfaceView present path end to end:
// host Mesa GL (gfx::GlDevice, Wayland backend) renders straight into acl's toplevel
// wl_surface and Mesa presents it (eglSwapBuffers), driven by wm::present_gl_loop. No GPL-3.0 sources.
//
// Run against a live compositor:  wm_gl_probe [frames] [title]
// A clearing animation (the background cycles) proves Mesa's Wayland WSI is presenting
// acl's GL frames — the native underside of an app's GLThread, before any JNI wiring.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "acl_config.h"

#if ACL_HAVE_GL && ACL_HAVE_WCS

#include <GLES2/gl2.h>

#include "gfx/gl_egl.h"
#include "wm/present.h"

int main(int argc, char** argv) {
  const std::uint64_t frames = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 120;
  const std::string title = argc > 2 ? argv[2] : "acl GL probe";
  constexpr int kW = 640, kH = 480;

  std::unique_ptr<acl::gfx::GlDevice> dev;
  std::unique_ptr<acl::gfx::GlWindow> win;

  acl::wm::GlPresentHooks hooks;
  hooks.configure = [&](void* wl_display, void* wl_surface, int w, int h) -> bool {
    std::string err;
    dev = acl::gfx::GlDevice::CreateWayland(wl_display, &err);
    if (!dev) { std::fprintf(stderr, "gl_probe: CreateWayland failed: %s\n", err.c_str()); return false; }
    std::fprintf(stderr, "gl_probe: GL device = %s\n", dev->renderer().c_str());
    win = dev->make_window(w, h, wl_surface, {}, &err);
    if (!win) { std::fprintf(stderr, "gl_probe: make_window failed: %s\n", err.c_str()); return false; }
    return true;
  };
  hooks.render = [&](std::uint64_t i) -> bool {
    std::string err;
    if (!win->make_current(&err)) { std::fprintf(stderr, "gl_probe: make_current: %s\n", err.c_str()); return false; }
    glViewport(0, 0, win->width(), win->height());
    const float t = static_cast<float>(i % 120) / 120.0f;   // cycle over ~2s @60fps
    glClearColor(0.15f + 0.5f * t, 0.25f, 0.6f - 0.4f * t, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!win->swap(nullptr, &err)) { std::fprintf(stderr, "gl_probe: swap: %s\n", err.c_str()); return false; }
    return true;
  };
  hooks.resize = [&](int w, int h) {
    if (win) { std::string e; win->resize(w, h, &e); }
  };
  hooks.teardown = [&] { win.reset(); dev.reset(); };  // before acl disconnects the wl_display

  const bool ok = acl::wm::present_gl_loop(kW, kH, title, "com.acl.glprobe", hooks,
                                           acl::wm::InputCallback{}, frames);
  std::fprintf(stderr, "gl_probe: %s\n", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}

#else
int main() {
  std::fprintf(stderr, "gl_probe: built without ACL_HAVE_GL/ACL_HAVE_WCS\n");
  return 77;  // skip
}
#endif

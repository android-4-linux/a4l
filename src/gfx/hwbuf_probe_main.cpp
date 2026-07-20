// SPDX-License-Identifier: Apache-2.0
// Provenance: EGL_EXT_image_dma_buf_import (EGL_LINUX_DMA_BUF_EXT + EGL_DMA_BUF_PLANE0_* attribs),
// GL_OES_EGL_image_external (samplerExternalOES, glEGLImageTargetTexture2DOES), Khronos EGL/GLES2
// headers, EGL_KHR_surfaceless_context. Validates gfx::HardwareBuffer end-to-end: it is the M1
// proof for the Flutter external-texture bridge ([[flutter-texture-bridge]]) — a CPU-written dma-buf
// imported as an EGLImage and sampled in GL must read back bit-identical (zero copy, no blit). No GPL-3.0 sources.
//
// Headless: a surfaceless GLES context on a DRM render node, so it needs no compositor and runs in
// CI where /dev/dri/renderD128 exists. Exit 0 = pass, 2 = skip (no GL/render node), 1 = fail.

#include "acl_config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !ACL_HAVE_GL
int main() { std::fprintf(stderr, "hwbuf-probe: built without GL support\n"); return 2; }
#else

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "gfx/gl_egl.h"
#include "gfx/hardware_buffer.h"

using acl::gfx::GlDevice;
using acl::gfx::HardwareBuffer;
using acl::gfx::HbFormat;

namespace {
constexpr int W = 64, H = 64;

// Deterministic test pattern written to the dma-buf (byte order R,G,B,A == DRM_FORMAT_ABGR8888).
void pattern(int x, int y, std::uint8_t px[4]) {
  px[0] = static_cast<std::uint8_t>(x * 4);  // R ramps with x
  px[1] = static_cast<std::uint8_t>(y * 4);  // G ramps with y
  px[2] = 128;                               // B constant
  px[3] = 255;                               // A opaque
}

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); std::fprintf(stderr, "shader: %s\n", log); return 0; }
  return s;
}
}  // namespace

int main() {
  std::string err;
  // A headless GLES device on the render node (no compositor). CreateGbm gives us display+context.
  auto dev = GlDevice::CreateGbm(&err);
  if (!dev) { std::fprintf(stderr, "hwbuf-probe: no GL device (%s) — skip\n", err.c_str()); return 2; }
  auto dpy = static_cast<EGLDisplay>(dev->egl_display());
  auto ctx = static_cast<EGLContext>(dev->egl_context());
  if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) != EGL_TRUE) {
    std::fprintf(stderr, "hwbuf-probe: surfaceless eglMakeCurrent failed (%#x) — skip\n", eglGetError());
    return 2;
  }

  // 1) Allocate an RGBA HardwareBuffer and CPU-fill the test pattern via lock().
  auto hb = HardwareBuffer::allocate(W, H, HbFormat::R8G8B8A8_UNORM, 0, &err);
  if (!hb) { std::fprintf(stderr, "hwbuf-probe: allocate failed (%s) — skip\n", err.c_str()); return 2; }
  std::uint32_t stride = 0;
  auto* base = static_cast<std::uint8_t*>(hb->lock(&stride, &err));
  if (base == nullptr) { std::fprintf(stderr, "hwbuf-probe: lock failed (%s)\n", err.c_str()); return 1; }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      pattern(x, y, base + static_cast<std::size_t>(y) * stride + x * 4);
  hb->unlock();

  // 2) Import the exported dma-buf as an EGLImage (zero copy — no CPU readback into GL).
  auto eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  auto eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  auto glEGLImageTargetTexture2DOES =
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (eglCreateImageKHR == nullptr || glEGLImageTargetTexture2DOES == nullptr) {
    std::fprintf(stderr, "hwbuf-probe: missing EGLImage/OES entry points — skip\n");
    return 2;
  }
  const auto& f = hb->frame();
  EGLint attrs[] = {
      EGL_WIDTH, W, EGL_HEIGHT, H,
      EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(f.drm_format),
      EGL_DMA_BUF_PLANE0_FD_EXT, f.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(f.planes[0].offset),
      EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(f.planes[0].stride),
      EGL_NONE};
  EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
  if (img == EGL_NO_IMAGE_KHR) { std::fprintf(stderr, "hwbuf-probe: eglCreateImageKHR failed (%#x)\n", eglGetError()); return 1; }

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (GLenum e = glGetError(); e != GL_NO_ERROR) { std::fprintf(stderr, "hwbuf-probe: bind OES texture failed (%#x)\n", e); return 1; }

  // 3) Sample the external texture into an offscreen RGBA FBO at 1:1.
  GLuint fbo = 0, color = 0;
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { std::fprintf(stderr, "hwbuf-probe: FBO incomplete\n"); return 1; }

  const char* vs = "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;"
                   "void main(){ vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }";
  const char* fs = "#extension GL_OES_EGL_image_external : require\n"
                   "precision mediump float; varying vec2 vTex; uniform samplerExternalOES uTex;"
                   "void main(){ gl_FragColor = texture2D(uTex, vTex); }";
  GLuint vsh = compile(GL_VERTEX_SHADER, vs), fsh = compile(GL_FRAGMENT_SHADER, fs);
  if (vsh == 0 || fsh == 0) return 1;
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vsh); glAttachShader(prog, fsh);
  glBindAttribLocation(prog, 0, "aPos"); glBindAttribLocation(prog, 1, "aTex");
  glLinkProgram(prog);
  GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked) { char log[512]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log); std::fprintf(stderr, "link: %s\n", log); return 1; }
  glUseProgram(prog);
  glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);

  const GLfloat verts[] = {  // pos.xy, tex.xy  (triangle strip)
      -1, -1, 0, 0,   1, -1, 1, 0,   -1, 1, 0, 1,   1, 1, 1, 1};
  glViewport(0, 0, W, H);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // 4) Read back and compare. glReadPixels is bottom-up; texcoord v=0 maps to FBO row 0, which
  // sampled dma-buf row 0 (top). So FBO row r == pattern row r (the strip already y-flips NDC vs
  // texcoord consistently). Verify a grid of texels bit-exact.
  std::vector<std::uint8_t> got(static_cast<std::size_t>(W) * H * 4);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got.data());
  if (GLenum e = glGetError(); e != GL_NO_ERROR) { std::fprintf(stderr, "hwbuf-probe: readback failed (%#x)\n", e); return 1; }

  int mismatches = 0, checked = 0;
  for (int y = 0; y < H; y += 8) {
    for (int x = 0; x < W; x += 8) {
      std::uint8_t want[4]; pattern(x, y, want);
      const std::uint8_t* g = &got[(static_cast<std::size_t>(y) * W + x) * 4];
      ++checked;
      if (std::memcmp(want, g, 4) != 0) {
        if (mismatches < 6)
          std::fprintf(stderr, "  (%2d,%2d) want %3d,%3d,%3d,%3d got %3d,%3d,%3d,%3d\n",
                       x, y, want[0], want[1], want[2], want[3], g[0], g[1], g[2], g[3]);
        ++mismatches;
      }
    }
  }

  eglDestroyImageKHR(dpy, img);
  if (mismatches != 0) {
    std::fprintf(stderr, "hwbuf-probe: FAIL — %d/%d sampled texels mismatched\n", mismatches, checked);
    return 1;
  }
  std::printf("hwbuf-probe: PASS — %dx%d dma-buf imported as EGLImage and sampled bit-exact (%d texels, stride=%u, mod=%#lx)\n",
              W, H, checked, hb->frame().planes[0].stride, static_cast<unsigned long>(hb->frame().modifier));
  return 0;
}

#endif  // ACL_HAVE_GL

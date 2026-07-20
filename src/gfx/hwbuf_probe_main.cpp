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

#include "gfx/ahardware_buffer.h"
#include "gfx/buffer_queue.h"
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
  std::shared_ptr<HardwareBuffer> hb = HardwareBuffer::allocate(W, H, HbFormat::R8G8B8A8_UNORM, 0, &err);
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
  // Set up the sampling harness ONCE: an OES texture, an offscreen RGBA FBO, the samplerExternalOES
  // program, and a 1:1 quad. Each check() rebinds the OES texture to a different EGLImage and reads
  // back, so both import paths run through identical sampling.
  GLuint tex = 0, fbo = 0, color = 0;
  glGenTextures(1, &tex);
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
  const GLfloat verts[] = {-1, -1, 0, 0,   1, -1, 1, 0,   -1, 1, 0, 1,   1, 1, 1, 1};
  glViewport(0, 0, W, H);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);

  // Bind `image` to the OES texture, draw the quad, read back, and compare bit-exact to the pattern.
  // Returns 0 on success, nonzero on failure/mismatch. glReadPixels row r == dma-buf row r (the
  // strip y-flips NDC vs texcoord consistently).
  auto check = [&](EGLImageKHR image, const char* label) -> int {
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (GLenum e = glGetError(); e != GL_NO_ERROR) { std::fprintf(stderr, "hwbuf-probe[%s]: bind OES failed (%#x)\n", label, e); return 1; }
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    std::vector<std::uint8_t> got(static_cast<std::size_t>(W) * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got.data());
    if (GLenum e = glGetError(); e != GL_NO_ERROR) { std::fprintf(stderr, "hwbuf-probe[%s]: readback failed (%#x)\n", label, e); return 1; }
    int mism = 0, checked = 0;
    for (int y = 0; y < H; y += 8)
      for (int x = 0; x < W; x += 8) {
        std::uint8_t want[4]; pattern(x, y, want);
        const std::uint8_t* g = &got[(static_cast<std::size_t>(y) * W + x) * 4];
        ++checked;
        if (std::memcmp(want, g, 4) != 0) {
          if (mism < 4) std::fprintf(stderr, "  [%s] (%2d,%2d) want %3d,%3d,%3d,%3d got %3d,%3d,%3d,%3d\n",
                                     label, x, y, want[0], want[1], want[2], want[3], g[0], g[1], g[2], g[3]);
          ++mism;
        }
      }
    if (mism != 0) { std::fprintf(stderr, "hwbuf-probe: FAIL [%s] — %d/%d texels mismatched\n", label, mism, checked); return 1; }
    std::printf("hwbuf-probe: PASS [%s] — %d texels sampled bit-exact\n", label, checked);
    return 0;
  };

  const auto& f = hb->frame();

  // Path A (M1): the engine's direct dma-buf import — eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT).
  EGLint attrs[] = {
      EGL_WIDTH, W, EGL_HEIGHT, H,
      EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(f.drm_format),
      EGL_DMA_BUF_PLANE0_FD_EXT, f.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(f.planes[0].offset),
      EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(f.planes[0].stride),
      EGL_NONE};
  EGLImageKHR img_a = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
  if (img_a == EGL_NO_IMAGE_KHR) { std::fprintf(stderr, "hwbuf-probe: direct import failed (%#x)\n", eglGetError()); return 1; }
  int rc = check(img_a, "M1 direct dma-buf import");

  // Path B (M2): wrap as an AHardwareBuffer and go through acl's translation — this is exactly what
  // the eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ahb) interceptor does for the engine.
  AHardwareBuffer* ahb = acl::gfx::ahb_create(hb);
  EGLImageKHR img_b = static_cast<EGLImageKHR>(acl::gfx::egl_image_from_ahb(
      dpy, ahb, nullptr, reinterpret_cast<acl::gfx::EglCreateImageFn>(eglCreateImageKHR)));
  if (img_b == EGL_NO_IMAGE_KHR) { std::fprintf(stderr, "hwbuf-probe: AHB translation failed (%#x)\n", eglGetError()); rc = 1; }
  else rc |= check(img_b, "M2 AHardwareBuffer->dma-buf translation");

  // Path C (M4 producer fill): the producer's own frame source. Refill the buffer a solid color via
  // ahb_fill_color and confirm the SAME EGLImage now samples it (CPU fill -> dma-buf -> texture).
  if (img_b != EGL_NO_IMAGE_KHR) {
    const std::uint32_t kFill = 0xFF3060C0;  // ARGB
    acl::gfx::ahb_fill_color(ahb, kFill);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img_b);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    std::uint8_t p[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    const std::uint8_t wr = (kFill >> 16) & 0xff, wg = (kFill >> 8) & 0xff, wb = kFill & 0xff;
    if (p[0] == wr && p[1] == wg && p[2] == wb)
      std::printf("hwbuf-probe: PASS [M4 producer fill] — solid %06x sampled\n", kFill & 0xffffff);
    else {
      std::fprintf(stderr, "hwbuf-probe: FAIL [M4 producer fill] want %02x%02x%02x got %02x%02x%02x\n",
                   wr, wg, wb, p[0], p[1], p[2]);
      rc = 1;
    }
  }

  if (img_b != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(dpy, img_b);
  eglDestroyImageKHR(dpy, img_a);
  acl::gfx::ahb_release(ahb);

  // Path D (M3c BufferQueue): the ImageReader pool. Produce a frame (dequeue -> fill -> queue), then
  // consume the latest (acquire) and sample it through the M2 translation — the full producer ->
  // consumer -> engine-texture flow the real ImageReader drives.
  {
    std::string qerr;
    int frame_cb = 0;
    auto q = acl::gfx::BufferQueue::create(W, H, static_cast<std::uint32_t>(HbFormat::R8G8B8A8_UNORM),
                                           0, 3, &qerr);
    if (!q) { std::fprintf(stderr, "hwbuf-probe: FAIL [M3c] BufferQueue create: %s\n", qerr.c_str()); return 1; }
    q->set_on_frame_available([&frame_cb] { ++frame_cb; });
    const std::uint32_t kQFill = 0xFF20C040;
    AHardwareBuffer* pb = q->dequeue();                 // producer: take a free buffer
    if (pb == nullptr) { std::fprintf(stderr, "hwbuf-probe: FAIL [M3c] dequeue null\n"); return 1; }
    acl::gfx::ahb_fill_color(pb, kQFill);
    q->queue(pb);                                        // producer: publish it
    AHardwareBuffer* cb2 = q->acquire_latest();          // consumer: take the latest
    int qrc = 1;
    if (cb2 != pb) std::fprintf(stderr, "hwbuf-probe: FAIL [M3c] acquire != produced\n");
    else if (frame_cb != 1) std::fprintf(stderr, "hwbuf-probe: FAIL [M3c] frame cb fired %d times\n", frame_cb);
    else {
      EGLImageKHR qi = static_cast<EGLImageKHR>(acl::gfx::egl_image_from_ahb(
          dpy, cb2, nullptr, reinterpret_cast<acl::gfx::EglCreateImageFn>(eglCreateImageKHR)));
      if (qi == EGL_NO_IMAGE_KHR) std::fprintf(stderr, "hwbuf-probe: FAIL [M3c] import\n");
      else {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, qi);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        std::uint8_t p[4] = {0, 0, 0, 0};
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        if (p[0] == ((kQFill >> 16) & 0xff) && p[1] == ((kQFill >> 8) & 0xff) && p[2] == (kQFill & 0xff))
          qrc = 0;
        else std::fprintf(stderr, "hwbuf-probe: FAIL [M3c] want %06x got %02x%02x%02x\n",
                          kQFill & 0xffffff, p[0], p[1], p[2]);
        eglDestroyImageKHR(dpy, qi);
      }
    }
    q->release(cb2);
    if (qrc == 0) std::printf("hwbuf-probe: PASS [M3c BufferQueue] — produce->acquire->sample %06x\n", kQFill & 0xffffff);
    else rc = 1;
  }
  // Path E (ZERO-COPY import — the video-decode path): a hardware decoder owns its output dma-bufs
  // (VAAPI/V4L2-M2M surfaces exported via av_hwframe_map to DRM_PRIME); acl imports each frame's
  // dma-buf with NO pixel copy and routes it to the consumer via BufferQueue::queue_imported. Proof:
  // CPU-fill a distinct color into a SOURCE buffer, ahb_import ITS dma-buf as a separate
  // AHardwareBuffer (the "decoder frame"), queue+acquire it, and sample. The imported buffer is never
  // written, so reading the source's color back proves the two share memory — the import is zero-copy.
  {
    const std::uint32_t kImp = 0xFF8040C0;  // ARGB, distinct from every other path
    const std::uint8_t ir = (kImp >> 16) & 0xff, ig = (kImp >> 8) & 0xff, ib = kImp & 0xff;
    int erc = 1;
    auto src = HardwareBuffer::allocate(W, H, HbFormat::R8G8B8A8_UNORM, 0, &err);  // stands in for a decoder surface
    std::uint32_t sstride = 0;
    std::uint8_t* sb = src ? static_cast<std::uint8_t*>(src->lock(&sstride, &err)) : nullptr;
    if (sb == nullptr) {
      std::fprintf(stderr, "hwbuf-probe: FAIL [E] source alloc/lock: %s\n", err.c_str());
      rc = 1;
    } else {
      for (int y = 0; y < H; ++y) {
        std::uint8_t* row = sb + static_cast<std::size_t>(y) * sstride;
        for (int x = 0; x < W; ++x) { row[0] = ir; row[1] = ig; row[2] = ib; row[3] = 255; row += 4; }
      }
      src->unlock();
      AHardwareBuffer* imp = acl::gfx::ahb_import(src->frame());  // import the SOURCE's dma-buf (fd dup, no copy)
      std::string qerr;
      auto q2 = acl::gfx::BufferQueue::create(W, H, static_cast<std::uint32_t>(HbFormat::R8G8B8A8_UNORM),
                                              0, 3, &qerr);
      if (imp == nullptr || !q2) {
        std::fprintf(stderr, "hwbuf-probe: FAIL [E] ahb_import/queue create\n");
      } else {
        q2->queue_imported(imp);  // hands the decoder frame to the consumer (transfers the ref)
        AHardwareBuffer* got = q2->acquire_latest();
        if (got != imp) {
          std::fprintf(stderr, "hwbuf-probe: FAIL [E] acquire_latest != imported\n");
        } else {
          EGLImageKHR ei = static_cast<EGLImageKHR>(acl::gfx::egl_image_from_ahb(
              dpy, got, nullptr, reinterpret_cast<acl::gfx::EglCreateImageFn>(eglCreateImageKHR)));
          if (ei == EGL_NO_IMAGE_KHR) {
            std::fprintf(stderr, "hwbuf-probe: FAIL [E] EGLImage import of imported buffer\n");
          } else {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, ei);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            std::uint8_t p[4] = {0, 0, 0, 0};
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
            if (p[0] == ir && p[1] == ig && p[2] == ib) erc = 0;
            else std::fprintf(stderr, "hwbuf-probe: FAIL [E] want %06x got %02x%02x%02x (import copied?)\n",
                              kImp & 0xffffff, p[0], p[1], p[2]);
            eglDestroyImageKHR(dpy, ei);
          }
        }
        q2->release(got);  // drops the queue's ref -> the imported buffer's dup fd is closed
      }
    }
    if (erc == 0)
      std::printf("hwbuf-probe: PASS [E zero-copy import] — decoder dma-buf imported + queued + "
                  "sampled %06x with NO copy\n", kImp & 0xffffff);
    else rc = 1;
  }

  if (rc == 0)
    std::printf("hwbuf-probe: ALL PASS — %dx%d, stride=%u, mod=%#lx\n", W, H, f.planes[0].stride,
                static_cast<unsigned long>(f.modifier));
  return rc;
}

#endif  // ACL_HAVE_GL

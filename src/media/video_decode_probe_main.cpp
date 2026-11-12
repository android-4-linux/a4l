// SPDX-License-Identifier: Apache-2.0
// Provenance: EGL_EXT_image_dma_buf_import + GL_OES_EGL_image_external
// (samplerExternalOES, the same sampling harness as gfx/hwbuf_probe_main.cpp),
// FFmpeg VAAPI decode (media::VideoDecoder). The zero-copy proof for the
// video-decode path ([[flutter-texture-bridge]] M5): hardware-decode a clip,
// map each frame to a DRM_PRIME dma-buf, ahb_import it (fd dup, NO pixel copy),
// and sample the multi-plane (NV12) EGLImage — Mesa does YUV->RGB in the
// sampler, so decode->texture is copy-free. Headless (surfaceless GLES on a
// render node). Exit 0 = pass, 2 = skip (no GL/VAAPI/FFmpeg/clip), 1 = fail.
// Usage: acl-video-decode-probe <clip> [expect: r,g,b of the clip's solid
// color].
//
// No GPL-3.0 sources.

#include "acl_config.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#if !ACL_HAVE_GL
int main() {
  std::fprintf(stderr, "video-decode-probe: built without GL support\n");
  return 2;
}
#else

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "gfx/ahardware_buffer.h"
#include "gfx/gl_egl.h"
#include "media/video_decoder.h"

using acl::gfx::GlDevice;

namespace {
GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    std::fprintf(stderr, "shader: %s\n", log);
    return 0;
  }
  return s;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: acl-video-decode-probe <clip> [r,g,b] [--packet]\n");
    return 2;
  }
  const char* clip = nullptr;
  int er = -1, eg = -1, eb = -1;
  bool packet_mode = false;  // --packet: exercise Demuxer -> CodecDecoder
                             // (MediaExtractor -> MediaCodec)
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--packet") == 0)
      packet_mode = true;
    else if (argv[i][0] == '-')
      continue;
    else if (clip == nullptr)
      clip = argv[i];
    else
      std::sscanf(argv[i], "%d,%d,%d", &er, &eg, &eb);
  }
  if (clip == nullptr) {
    std::fprintf(stderr,
                 "usage: acl-video-decode-probe <clip> [r,g,b] [--packet]\n");
    return 2;
  }

  std::string err;
  // Two frame sources sharing the zero-copy output: the file decoder
  // (VideoDecoder = demux+decode) and the packet path (Demuxer + CodecDecoder =
  // MediaExtractor + MediaCodec). Both yield DmabufFrames.
  std::unique_ptr<acl::media::VideoDecoder> fdec;
  std::unique_ptr<acl::media::Demuxer> demux;
  std::unique_ptr<acl::media::CodecDecoder> cdec;
  int W = 0, H = 0;
  if (packet_mode) {
    demux = acl::media::Demuxer::open(clip, &err);
    if (!demux) {
      std::fprintf(stderr,
                   "video-decode-probe: demux open failed (%s) — skip\n",
                   err.c_str());
      return 2;
    }
    W = demux->width();
    H = demux->height();
    cdec = acl::media::CodecDecoder::configure(
        demux->mime(), demux->extradata(), demux->extradata_size(), W, H, &err);
    if (!cdec) {
      std::fprintf(stderr,
                   "video-decode-probe: codec configure failed (%s) — skip\n",
                   err.c_str());
      return 2;
    }
  } else {
    fdec = acl::media::VideoDecoder::open(clip, &err);
    if (!fdec) {
      std::fprintf(stderr, "video-decode-probe: open failed (%s) — skip\n",
                   err.c_str());
      return 2;
    }
    W = static_cast<int>(fdec->width());
    H = static_cast<int>(fdec->height());
  }
  if (W <= 0 || H <= 0) {
    std::fprintf(stderr, "video-decode-probe: bad dims %dx%d — skip\n", W, H);
    return 2;
  }

  auto dev = GlDevice::CreateGbm(&err);
  if (!dev) {
    std::fprintf(stderr, "video-decode-probe: no GL device (%s) — skip\n",
                 err.c_str());
    return 2;
  }
  auto dpy = static_cast<EGLDisplay>(dev->egl_display());
  auto ctx = static_cast<EGLContext>(dev->egl_context());
  if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) != EGL_TRUE) {
    std::fprintf(stderr,
                 "video-decode-probe: eglMakeCurrent failed (%#x) — skip\n",
                 eglGetError());
    return 2;
  }
  auto eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
      eglGetProcAddress("eglCreateImageKHR"));
  auto eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
      eglGetProcAddress("eglDestroyImageKHR"));
  auto glEGLImageTargetTexture2DOES =
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
          eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (eglCreateImageKHR == nullptr || glEGLImageTargetTexture2DOES == nullptr) {
    std::fprintf(
        stderr,
        "video-decode-probe: missing EGLImage/OES entry points — skip\n");
    return 2;
  }

  // OES sampling harness: an external texture -> RGBA FBO, samplerExternalOES
  // program, 1:1 quad.
  GLuint tex = 0, fbo = 0, color = 0;
  glGenTextures(1, &tex);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         color, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr, "video-decode-probe: FBO incomplete\n");
    return 1;
  }
  const char* vs =
      "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;"
      "void main(){ vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }";
  const char* fs =
      "#extension GL_OES_EGL_image_external : require\n"
      "precision mediump float; varying vec2 vTex; uniform samplerExternalOES "
      "uTex;"
      "void main(){ gl_FragColor = texture2D(uTex, vTex); }";
  GLuint vsh = compile(GL_VERTEX_SHADER, vs),
         fsh = compile(GL_FRAGMENT_SHADER, fs);
  if (vsh == 0 || fsh == 0)
    return 1;
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vsh);
  glAttachShader(prog, fsh);
  glBindAttribLocation(prog, 0, "aPos");
  glBindAttribLocation(prog, 1, "aTex");
  glLinkProgram(prog);
  GLint linked = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[512];
    glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
    std::fprintf(stderr, "link: %s\n", log);
    return 1;
  }
  glUseProgram(prog);
  glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
  glActiveTexture(GL_TEXTURE0);
  const GLfloat verts[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
  glViewport(0, 0, W, H);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        verts + 2);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);

  // Decode + import + sample the first frame. The frame's dma-buf is the
  // DECODER's output surface, imported zero-copy (ahb_import dups the fd — no
  // pixel copy); the engine samples it identically.
  acl::gfx::DmabufFrame f{};
  int r = 0;
  if (packet_mode) {
    // MediaExtractor -> MediaCodec: pump access units until the first frame
    // emerges (then drain).
    bool eos = false;
    for (int guard = 0; guard < 4000 && r == 0; ++guard) {
      if (!eos) {
        const std::uint8_t* pd = nullptr;
        std::size_t ps = 0;
        std::int64_t pts = 0;
        int rs = demux->read_sample(&pd, &ps, &pts, &err);
        if (rs < 0) {
          std::fprintf(stderr, "video-decode-probe: read_sample failed (%s)\n",
                       err.c_str());
          return 1;
        }
        if (rs == 0) {
          eos = true;
          cdec->send_packet(nullptr, 0, 0, &err);
        }  // EOS drain
        else if (cdec->send_packet(pd, ps, pts, &err) < 0) {
          std::fprintf(stderr, "video-decode-probe: send_packet failed (%s)\n",
                       err.c_str());
          return 1;
        }
      }
      r = cdec->receive_frame(&f, nullptr, &err);
      if (r < 0) {
        std::fprintf(stderr, "video-decode-probe: receive_frame failed (%s)\n",
                     err.c_str());
        return 1;
      }
      if (eos && r == 0)
        break;  // fully drained, no frame
    }
  } else {
    r = fdec->next_frame(&f, &err);
  }
  if (r <= 0) {
    std::fprintf(stderr, "video-decode-probe: no frame (r=%d %s) — %s\n", r,
                 err.c_str(), r == 0 ? "empty; skip" : "fail");
    return r == 0 ? 2 : 1;
  }
  std::printf(
      "video-decode-probe: mode=%s\n",
      packet_mode ? "packet (Demuxer+CodecDecoder)" : "file (VideoDecoder)");
  std::printf(
      "video-decode-probe: decoded %dx%d fourcc=%#x planes=%u mod=%#lx fd=%d\n",
      f.width, f.height, f.drm_format, f.plane_count,
      static_cast<unsigned long>(f.modifier), f.fd);

  AHardwareBuffer* ahb = acl::gfx::ahb_import(
      f);  // zero-copy: dup the decoder's fd + record layout
  if (ahb == nullptr) {
    std::fprintf(stderr, "video-decode-probe: ahb_import failed\n");
    return 1;
  }
  EGLImageKHR img = static_cast<EGLImageKHR>(acl::gfx::egl_image_from_ahb(
      dpy, ahb, nullptr,
      reinterpret_cast<acl::gfx::EglCreateImageFn>(eglCreateImageKHR)));
  if (img == EGL_NO_IMAGE_KHR) {
    std::fprintf(
        stderr,
        "video-decode-probe: EGLImage import of decoded frame failed (%#x) — "
        "the "
        "multi-plane NV12 attribs / driver may not support this modifier\n",
        eglGetError());
    acl::gfx::ahb_release(ahb);
    return 1;
  }
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (GLenum e = glGetError(); e != GL_NO_ERROR) {
    std::fprintf(stderr, "video-decode-probe: bind OES failed (%#x)\n", e);
    return 1;
  }
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  std::uint8_t p[4] = {0, 0, 0, 0};
  glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
  eglDestroyImageKHR(dpy, img);
  acl::gfx::ahb_release(ahb);
  if (GLenum e = glGetError(); e != GL_NO_ERROR) {
    std::fprintf(stderr, "video-decode-probe: sample failed (%#x)\n", e);
    return 1;
  }

  std::printf("video-decode-probe: center pixel = %d,%d,%d\n", p[0], p[1],
              p[2]);
  const bool non_black =
      p[0] + p[1] + p[2] > 24;  // a real decoded frame is not all-black
  if (!non_black) {
    std::fprintf(
        stderr,
        "video-decode-probe: FAIL — sampled black (decode/import broken)\n");
    return 1;
  }
  if (er >= 0) {
    // Solid-color clip: verify the sampled color matches (generous tolerance
    // for YUV 4:2:0 round-trip
    // + colorspace matrix). The dominant channel and rough magnitude must
    // survive decode->import->sample.
    const int tol = 56;
    if (std::abs(p[0] - er) > tol || std::abs(p[1] - eg) > tol ||
        std::abs(p[2] - eb) > tol) {
      std::fprintf(
          stderr,
          "video-decode-probe: FAIL — want ~%d,%d,%d got %d,%d,%d (tol %d)\n",
          er, eg, eb, p[0], p[1], p[2], tol);
      return 1;
    }
    std::printf(
        "video-decode-probe: PASS — decoded solid %d,%d,%d recovered as "
        "%d,%d,%d "
        "(zero-copy HW decode -> DRM_PRIME dma-buf -> import -> NV12 sample)\n",
        er, eg, eb, p[0], p[1], p[2]);
  } else {
    std::printf(
        "video-decode-probe: PASS — decoded frame imported + sampled zero-copy "
        "(HW decode -> DRM_PRIME dma-buf -> ahb_import -> NV12 EGLImage)\n");
  }
  return 0;
}

#endif  // ACL_HAVE_GL

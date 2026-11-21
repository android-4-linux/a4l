// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl work. No GPL-3.0 sources.
//
// Dev tool: a hardware-decoded clip on the video plane of a real compositor,
// with no app and no VM. An Annex B file with an access unit delimiter before
// every frame is decoded by media::DmaCodec, and each frame goes to the
// presenter's video plane under a transparent window, as a video
// SurfaceView's does. Run by hand against a live compositor; what it shows is
// read off a screenshot. A clip:
//
//   ffmpeg -f lavfi -i testsrc=size=640x360:rate=30 -frames:v 60
//     -c:v libx265 -x265-params bframes=0
//     -bsf:v hevc_metadata=aud=insert -f hevc clip.h265
//
//   --size WxH   the clip's picture size (640x360)
//   --split      the frames cross the --split boundary: this thread is the
//                app process's side (the proxy presenter), a second thread is
//                the broker with the real presenter, a socket pair between
//                them. The broker's count of frames taken and refused is
//                printed.
//   --planes     ask the decoder for a buffer per plane, where it has the
//                choice (a4l asks for one). The frame is then the size the
//                decoder reports, padding included.
//   --software   decode with FFmpeg in software (media::CodecDecoder with
//                ACL_HWDEC_DEVICE=none): each frame is copied into a
//                dma-buf, NV12 or, for 10 bits, P010
//   --loops N    play the clip N times (3)
//   --fps N      frames a second (30)
//   --window HEX the window's fill as ARGB (00000000, so the plane shows;
//                ff204060 covers it)
//
// Exit status: 0 if frames were decoded and sent (and, with --split, the
// broker took some), 2 if there is no decoder, clip or display to try.

#include <poll.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "acl_config.h"

#if ACL_HAVE_WCS && ACL_HAVE_VULKAN && ACL_HAVE_DMABUF_CODEC

#include <linux/videodev2.h>

#include "gfx/gfx_vk.h"
#include "ipc/ipc.h"
#include "media/dma_codec.h"
#if ACL_HAVE_FFMPEG && ACL_HAVE_GL
#include "media/video_decoder.h"
#endif
#include "src/dma_decoder_factory.h"
#include "wm/present.h"
#include "wm/split_present.h"
#include "wm/split_validate.h"

namespace {

// Where each access unit starts, then the end of the file. An access unit
// runs from one delimiter to the next.
std::vector<std::size_t> access_units(const std::vector<std::uint8_t>& b,
                                      bool hevc) {
  std::vector<std::size_t> at;
  for (std::size_t i = 0; i + 4 < b.size(); ++i) {
    if (b[i] != 0 || b[i + 1] != 0)
      continue;
    std::size_t h = 0;
    if (b[i + 2] == 1)
      h = i + 3;
    else if (b[i + 2] == 0 && b[i + 3] == 1)
      h = i + 4;
    else
      continue;
    if (h >= b.size())
      break;
    const bool aud = hevc ? ((b[h] >> 1) & 0x3f) == 35 : (b[h] & 0x1f) == 9;
    if (aud)
      at.push_back(i);
    i = h;
  }
  if (!at.empty())
    at.push_back(b.size());
  return at;
}

// Handles what the presenter has to read until `ms` have passed.
void pump(acl::wm::IPresenter& p, int ms) {
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          until - std::chrono::steady_clock::now())
                          .count();
    pollfd pfd{p.fd(), POLLIN, 0};
    if (::poll(&pfd, 1, left > 0 ? static_cast<int>(left) : 0) > 0)
      p.dispatch();
    if (left <= 0)
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* clip = nullptr;
  bool split = false, planes = false, software = false;
  int w = 640, h = 360, loops = 3, fps = 30;
  std::uint32_t window_argb = 0;  // the window's fill; transparent
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--split") == 0)
      split = true;
    else if (std::strcmp(argv[i], "--planes") == 0)
      planes = true;
    else if (std::strcmp(argv[i], "--software") == 0)
      software = true;
    else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
      std::sscanf(argv[++i], "%dx%d", &w, &h);
    else if (std::strcmp(argv[i], "--loops") == 0 && i + 1 < argc)
      loops = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc)
      window_argb =
          static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 16));
    else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
      fps = std::atoi(argv[++i]);
    else
      clip = argv[i];
  }
  if (clip == nullptr || w <= 0 || h <= 0 || fps <= 0) {
    std::fprintf(stderr,
                 "usage: acl-video-plane-probe <clip.h265|clip.h264> "
                 "[--size WxH] [--split] [--planes] [--software] [--loops N] "
                 "[--fps N] [--window ARGB]\n");
    return 2;
  }
  const std::string name = clip;
  const bool hevc = name.size() > 4 && name.rfind("265") == name.size() - 3;
  std::ifstream in(clip, std::ios::binary);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  const std::vector<std::size_t> units = access_units(bytes, hevc);
  if (units.size() < 2) {
    std::fprintf(stderr,
                 "video-plane-probe: cannot read %s, or it has no "
                 "access unit delimiters\n",
                 clip);
    return 2;
  }

  std::string err;
  std::unique_ptr<acl::media::DmaCodec> codec;
#if ACL_HAVE_FFMPEG && ACL_HAVE_GL
  std::unique_ptr<acl::media::CodecDecoder> sw;
#endif
  if (software) {
#if ACL_HAVE_FFMPEG && ACL_HAVE_GL
    // A device FFmpeg does not know: it decodes in software, and the other
    // engines are not tried.
    ::setenv("ACL_HWDEC_DEVICE", "none", 1);
    sw = acl::media::CodecDecoder::configure(hevc ? "video/hevc" : "video/avc",
                                             nullptr, 0, w, h, &err);
#else
    err = "built without FFmpeg";
#endif
  } else if (planes) {
    const std::uint32_t fourcc = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    codec = acl::media::DmaCodec::with_factory(
        [fourcc, w, h] {
          v4l2wc::DmaDecoderConfig config;
          config.codec_fourcc = fourcc;
          config.coded_width = static_cast<std::uint32_t>(w);
          config.coded_height = static_cast<std::uint32_t>(h);
          return v4l2wc::CreateDmaDecoder(config);
        },
        nullptr, 0, &err);
  } else {
    codec = acl::media::DmaCodec::configure(hevc ? "video/hevc" : "video/avc",
                                            nullptr, 0, w, h, &err);
  }
  // The engine either way.
  const auto send = [&](const std::uint8_t* data, std::size_t size,
                        std::int64_t pts) {
#if ACL_HAVE_FFMPEG && ACL_HAVE_GL
    if (sw)
      return sw->send_packet(data, size, pts, &err);
#endif
    return codec->send_packet(data, size, pts, &err);
  };
  const auto receive = [&](acl::gfx::DmabufFrame* f) {
#if ACL_HAVE_FFMPEG && ACL_HAVE_GL
    if (sw)
      return sw->receive_frame(f, nullptr, &err);
#endif
    return codec->receive_frame(f, nullptr, &err);
  };
  bool have = codec != nullptr;
#if ACL_HAVE_FFMPEG && ACL_HAVE_GL
  have = have || sw != nullptr;
#endif
  if (!have) {
    std::fprintf(stderr, "video-plane-probe: no decoder (%s)\n", err.c_str());
    return 2;
  }

  // The window: transparent, so the plane under it shows, with one opaque
  // square to say the window is there and above.
  auto target = acl::gfx::VkRenderTarget::Create(w, h, &err);
  if (!target) {
    std::fprintf(stderr, "video-plane-probe: no Vulkan render target: %s\n",
                 err.c_str());
    return 2;
  }

  auto pair = acl::ipc::Channel::make_pair();
  std::thread broker;
  acl::wm::BrokerStats stats;
  std::unique_ptr<acl::wm::IPresenter> p;
  if (split) {
    broker = std::thread([&] {
      stats = acl::wm::serve_split_child(
          pair.second,
          [](int pw, int ph, std::string_view title, std::string_view app_id,
             acl::wm::InputCallback input, bool want_ime) {
            return acl::wm::presenter_open(pw, ph, title, app_id,
                                           std::move(input), want_ime);
          },
          {}, /*linger_ms=*/500);
    });
    p = acl::wm::split_presenter_open_on(pair.first, w, h, "a4l video plane",
                                         "org.a4l.videoplane", {}, false);
  } else {
    p = acl::wm::presenter_open(w, h, "a4l video plane", "org.a4l.videoplane",
                                {}, false);
  }
  int rc = 1;
  int shown = 0;
  bool refused = false;
  if (!p) {
    std::fprintf(stderr, "video-plane-probe: no presenter\n");
    rc = 2;
  } else {
    p->wait_configured(3000);
    for (int i = 0; i < 300 && !p->ready(); ++i)
      pump(*p, 10);
    acl::gfx::DmabufFrame win{};
    const std::uint32_t kClear = window_argb, kMark = 0xFFFFFFFF;
    const bool drawn =
        (target->modifier_tiled_available() && !p->modifiers().empty() &&
         target->render_to_modifier_dmabuf(p->modifiers(), kClear, 8, 8, 40, 40,
                                           kMark, &win, &err)) ||
        target->render_and_export_linear(kClear, 8, 8, 40, 40, kMark, &win,
                                         &err);
    // The render target calls its pixels XRGB. They are BGRA with the alpha
    // it drew, which is what lets the plane show through.
    win.drm_format = 0x34325241;  // ARGB8888
    if (!drawn || !p->submit(win, p->next_buffer()))
      std::fprintf(stderr, "video-plane-probe: no window frame (%s)\n",
                   err.c_str());
    p->set_video_bounds(0, 0, w, h);

    const auto frame_time = std::chrono::microseconds(1'000'000 / fps);
    auto due = std::chrono::steady_clock::now();
    const auto show = [&](const acl::gfx::DmabufFrame& f) {
      if (shown == 0)
        std::fprintf(stderr,
                     "video-plane-probe: frames %ux%u format 0x%x modifier "
                     "0x%llx, %s\n",
                     f.width, f.height, f.drm_format,
                     static_cast<unsigned long long>(f.modifier),
                     f.one_buffer() ? "one buffer" : "a buffer per plane");
      p->present_video(f, 0, 0, w, h);
      p->pump_video();
      ++shown;
      due += frame_time;
      const auto now = std::chrono::steady_clock::now();
      const int ms =
          due > now ? static_cast<int>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              due - now)
                              .count())
                    : 0;
      pump(*p, ms);
    };
    bool failed = false;
    for (int loop = 0; loop < loops && !failed && !p->closed(); ++loop) {
      for (std::size_t u = 0; u + 1 < units.size() && !failed; ++u) {
        const std::uint8_t* data = bytes.data() + units[u];
        const std::size_t size = units[u + 1] - units[u];
        for (int tries = 0; tries < 200; ++tries) {
          const int sent =
              send(data, size, static_cast<std::int64_t>(shown) * 33'333);
          acl::gfx::DmabufFrame f{};
          int got;
          while ((got = receive(&f)) == 1) {
            show(f);
            f = {};
          }
          if (sent < 0 || got < 0) {
            std::fprintf(stderr, "video-plane-probe: decode failed (%s)\n",
                         err.c_str());
            failed = true;
          }
          if (sent != 1)
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }
    }
    // What is still in the decoder.
    send(nullptr, 0, 0);
    for (int i = 0; i < 100; ++i) {
      acl::gfx::DmabufFrame f{};
      if (receive(&f) != 1)
        break;
      show(f);
    }
    // The last picture stays up long enough to be looked at.
    for (int i = 0; i < 100; ++i)
      pump(*p, 20);
    refused = p->video_refused(0x3231564e) || p->video_refused(0x30313050) ||
              p->video_refused(0x30333050);
    rc = failed || shown == 0 ? 1 : 0;
  }
  p.reset();
  codec.reset();
#if ACL_HAVE_FFMPEG && ACL_HAVE_GL
  sw.reset();
#endif
  if (split) {
    pair.first.close();
    broker.join();
    std::fprintf(stderr,
                 "video-plane-probe: broker took %llu video frame(s), refused "
                 "%llu message(s)\n",
                 static_cast<unsigned long long>(stats.video_frames),
                 static_cast<unsigned long long>(stats.rejected));
    for (int i = 0; i < acl::wm::kFrameVerdictCount; ++i) {
      if (stats.frame_verdicts[i] != 0)
        std::fprintf(
            stderr, "video-plane-probe:   %llu refused: %s\n",
            static_cast<unsigned long long>(stats.frame_verdicts[i]),
            acl::wm::verdict_name(static_cast<acl::wm::FrameVerdict>(i)));
    }
    if (rc == 0 && stats.video_frames == 0)
      rc = 1;
  }
  std::fprintf(stderr, "video-plane-probe: %d frame(s) sent to the plane%s\n",
               shown, refused ? "; the compositor refused the format" : "");
  return rc;
}

#else
int main() {
  std::fprintf(stderr,
               "video-plane-probe: built without the Wayland "
               "presenter, Vulkan or dmabuf-codec; nothing to do\n");
  return 0;
}
#endif

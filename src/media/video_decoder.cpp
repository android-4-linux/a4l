// SPDX-License-Identifier: Apache-2.0
// Provenance: FFmpeg public API (libav*, LGPL, dynamic link only — dlopen'd RTLD_LOCAL, per plan.md
// policy and mirroring src/audio/soundpool.cpp's FFmpeg isolation), the VAAPI hwaccel setup
// (avcodec_get_hw_config + AV_HWDEVICE_TYPE_VAAPI + a get_format callback) and DRM_PRIME export
// (av_hwframe_map AV_HWFRAME_MAP_DIRECT -> AVDRMFrameDescriptor, hwcontext_drm.h). No GPL-3.0 sources.

#include "media/video_decoder.h"

#if ACL_HAVE_GL

#include <dlfcn.h>
#include <drm_fourcc.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
}

#include "log/log.h"

namespace acl::media {
namespace {
constexpr const char* kTag = "acl.media.video";

// ── FFmpeg resolved at runtime (RTLD_LOCAL), like soundpool — keeps its transitive deps private.
struct Ff {
#define FF_FN(n) decltype(&::n) n = nullptr
  FF_FN(av_frame_alloc); FF_FN(av_frame_free); FF_FN(av_frame_unref);
  FF_FN(av_packet_alloc); FF_FN(av_packet_free); FF_FN(av_packet_unref);
  FF_FN(av_buffer_ref); FF_FN(av_buffer_unref);
  FF_FN(avformat_open_input); FF_FN(avformat_find_stream_info); FF_FN(avformat_close_input);
  FF_FN(av_find_best_stream); FF_FN(av_read_frame);
  FF_FN(avcodec_find_decoder); FF_FN(avcodec_alloc_context3); FF_FN(avcodec_parameters_to_context);
  FF_FN(avcodec_open2); FF_FN(avcodec_free_context); FF_FN(avcodec_get_hw_config);
  FF_FN(avcodec_send_packet); FF_FN(avcodec_receive_frame);
  FF_FN(av_hwdevice_ctx_create); FF_FN(av_hwframe_map);
#undef FF_FN
  bool ok = false;
};
Ff g_ff;
bool g_ff_tried = false;

void* ff_open(const char* base, int maj) {
  char v[64];
  std::snprintf(v, sizeof(v), "%s.%d", base, maj);
  if (void* h = ::dlopen(v, RTLD_NOW | RTLD_LOCAL)) return h;
  return ::dlopen(base, RTLD_NOW | RTLD_LOCAL);
}

bool load_ffmpeg() {
  if (g_ff_tried) return g_ff.ok;
  g_ff_tried = true;
  void* h[3] = {
      ff_open("libavutil.so", LIBAVUTIL_VERSION_MAJOR),
      ff_open("libavcodec.so", LIBAVCODEC_VERSION_MAJOR),
      ff_open("libavformat.so", LIBAVFORMAT_VERSION_MAJOR),
  };
  for (void* x : h) if (x == nullptr) { log::w(kTag, "FFmpeg dlopen failed; video decode off"); return false; }
  auto sym = [&](const char* n) -> void* {
    for (void* x : h) if (void* s = ::dlsym(x, n)) return s;
    return nullptr;
  };
  bool all = true;
#define FF_BIND(n) do { g_ff.n = reinterpret_cast<decltype(g_ff.n)>(sym(#n)); if (!g_ff.n) { all = false; log::w(kTag, "FFmpeg missing {}", #n); } } while (0)
  FF_BIND(av_frame_alloc); FF_BIND(av_frame_free); FF_BIND(av_frame_unref);
  FF_BIND(av_packet_alloc); FF_BIND(av_packet_free); FF_BIND(av_packet_unref);
  FF_BIND(av_buffer_ref); FF_BIND(av_buffer_unref);
  FF_BIND(avformat_open_input); FF_BIND(avformat_find_stream_info); FF_BIND(avformat_close_input);
  FF_BIND(av_find_best_stream); FF_BIND(av_read_frame);
  FF_BIND(avcodec_find_decoder); FF_BIND(avcodec_alloc_context3); FF_BIND(avcodec_parameters_to_context);
  FF_BIND(avcodec_open2); FF_BIND(avcodec_free_context); FF_BIND(avcodec_get_hw_config);
  FF_BIND(avcodec_send_packet); FF_BIND(avcodec_receive_frame);
  FF_BIND(av_hwdevice_ctx_create); FF_BIND(av_hwframe_map);
#undef FF_BIND
  g_ff.ok = all;
  log::i(kTag, "FFmpeg video decode {}", all ? "ready (VAAPI, RTLD_LOCAL)" : "unavailable");
  return all;
}

// The decoder's negotiated hardware pixel format, stashed in ctx->opaque so get_format can return it.
enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
  const auto want = static_cast<enum AVPixelFormat>(reinterpret_cast<std::intptr_t>(ctx->opaque));
  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
    if (*p == want) return *p;
  log::w(kTag, "get_format: hw pixfmt not offered; falling back to software");
  return fmts[0];
}
}  // namespace

struct VideoDecoder::Impl {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  AVBufferRef* hw_device = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* hw_frame = nullptr;   // the decoded VAAPI surface
  AVFrame* drm_frame = nullptr;  // hw_frame mapped to DRM_PRIME (its descriptor owns the dma-buf fds)
  int stream_index = -1;
  std::uint32_t width = 0, height = 0;

  ~Impl() {
    if (drm_frame) g_ff.av_frame_free(&drm_frame);
    if (hw_frame) g_ff.av_frame_free(&hw_frame);
    if (pkt) g_ff.av_packet_free(&pkt);
    if (codec) g_ff.avcodec_free_context(&codec);
    if (hw_device) g_ff.av_buffer_unref(&hw_device);
    if (fmt) g_ff.avformat_close_input(&fmt);
  }
};

VideoDecoder::VideoDecoder() : d_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;
std::uint32_t VideoDecoder::width() const { return d_->width; }
std::uint32_t VideoDecoder::height() const { return d_->height; }

std::unique_ptr<VideoDecoder> VideoDecoder::open(const std::string& path, std::string* error) {
  auto fail = [&](const char* m) -> std::unique_ptr<VideoDecoder> { if (error) *error = m; return nullptr; };
  if (!load_ffmpeg()) return fail("FFmpeg unavailable");

  auto dec = std::unique_ptr<VideoDecoder>(new VideoDecoder());
  Impl* d = dec->d_.get();

  if (g_ff.avformat_open_input(&d->fmt, path.c_str(), nullptr, nullptr) < 0) return fail("open_input failed");
  if (g_ff.avformat_find_stream_info(d->fmt, nullptr) < 0) return fail("find_stream_info failed");

  const AVCodec* codec = nullptr;
  d->stream_index = g_ff.av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (d->stream_index < 0 || codec == nullptr) return fail("no video stream");

  // Find the VAAPI hw config for this codec (the hw pixfmt to negotiate in get_format).
  enum AVPixelFormat hw_pixfmt = AV_PIX_FMT_NONE;
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = g_ff.avcodec_get_hw_config(codec, i);
    if (cfg == nullptr) break;
    if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AV_HWDEVICE_TYPE_VAAPI) {
      hw_pixfmt = cfg->pix_fmt;
      break;
    }
  }
  if (hw_pixfmt == AV_PIX_FMT_NONE) return fail("codec has no VAAPI hwaccel");

  d->codec = g_ff.avcodec_alloc_context3(codec);
  if (d->codec == nullptr) return fail("alloc codec context failed");
  if (g_ff.avcodec_parameters_to_context(d->codec, d->fmt->streams[d->stream_index]->codecpar) < 0)
    return fail("parameters_to_context failed");

  // VAAPI device (renderD128 unless ACL_DRM_RENDER_NODE overrides). Zero-copy decode target.
  const char* node = std::getenv("ACL_DRM_RENDER_NODE");
  if (g_ff.av_hwdevice_ctx_create(&d->hw_device, AV_HWDEVICE_TYPE_VAAPI,
                                  (node && *node) ? node : nullptr, nullptr, 0) < 0)
    return fail("av_hwdevice_ctx_create(VAAPI) failed");
  d->codec->hw_device_ctx = g_ff.av_buffer_ref(d->hw_device);
  d->codec->opaque = reinterpret_cast<void*>(static_cast<std::intptr_t>(hw_pixfmt));
  d->codec->get_format = get_hw_format;

  if (g_ff.avcodec_open2(d->codec, codec, nullptr) < 0) return fail("avcodec_open2 failed");
  d->width = static_cast<std::uint32_t>(d->codec->width);
  d->height = static_cast<std::uint32_t>(d->codec->height);

  d->pkt = g_ff.av_packet_alloc();
  d->hw_frame = g_ff.av_frame_alloc();
  d->drm_frame = g_ff.av_frame_alloc();
  if (!d->pkt || !d->hw_frame || !d->drm_frame) return fail("frame/packet alloc failed");

  log::i(kTag, "opened {}x{} VAAPI decode (stream {})", d->width, d->height, d->stream_index);
  return dec;
}

int VideoDecoder::next_frame(gfx::DmabufFrame* out, std::string* error) {
  auto err = [&](const char* m) -> int { if (error) *error = m; return -1; };
  if (out == nullptr) return err("null out");
  Impl* d = d_.get();

  // Release the previous frame's DRM mapping (its dma-buf fds) before decoding the next.
  g_ff.av_frame_unref(d->drm_frame);
  g_ff.av_frame_unref(d->hw_frame);

  for (;;) {
    int r = g_ff.avcodec_receive_frame(d->codec, d->hw_frame);
    if (r == 0) break;                       // got a decoded surface
    if (r == AVERROR_EOF) return 0;          // end of stream
    if (r != AVERROR(EAGAIN)) return err("avcodec_receive_frame failed");
    // Need more input: read + send the next video packet (or drain at EOF).
    int rr = g_ff.av_read_frame(d->fmt, d->pkt);
    if (rr == AVERROR_EOF) { g_ff.avcodec_send_packet(d->codec, nullptr); continue; }  // flush
    if (rr < 0) return err("av_read_frame failed");
    if (d->pkt->stream_index == d->stream_index) {
      const int sr = g_ff.avcodec_send_packet(d->codec, d->pkt);
      g_ff.av_packet_unref(d->pkt);
      if (sr < 0 && sr != AVERROR(EAGAIN)) return err("avcodec_send_packet failed");
    } else {
      g_ff.av_packet_unref(d->pkt);
    }
  }

  // Map the VAAPI surface to a DRM_PRIME dma-buf — DIRECT = zero-copy (no download to system memory).
  d->drm_frame->format = AV_PIX_FMT_DRM_PRIME;
  if (g_ff.av_hwframe_map(d->drm_frame, d->hw_frame, AV_HWFRAME_MAP_DIRECT | AV_HWFRAME_MAP_READ) < 0)
    return err("av_hwframe_map(DRM_PRIME) failed");
  const auto* desc = reinterpret_cast<const AVDRMFrameDescriptor*>(d->drm_frame->data[0]);
  if (desc == nullptr || desc->nb_objects < 1 || desc->nb_layers < 1) return err("empty DRM descriptor");

  // Single-object layout (the common VAAPI/V4L2-M2M case): all planes share objects[0].fd at offsets.
  // (Multi-object frames would need a per-plane-fd DmabufFrame — a later model change; reject for now.)
  if (desc->nb_objects != 1) return err("multi-object DRM frame (per-plane fds) not yet supported");

  gfx::DmabufFrame f{};
  f.fd = desc->objects[0].fd;  // owned by drm_frame; caller must ahb_import (dup) before next_frame
  f.width = d->width;
  f.height = d->height;
  f.modifier = desc->objects[0].format_modifier;
  f.modifier_tiled = f.modifier != 0 && f.modifier != DRM_FORMAT_MOD_INVALID;

  // Flatten EVERY layer's planes into the frame: VAAPI/radeonsi exports NV12 as TWO layers of the one
  // object (layer0 = R8 luma, layer1 = GR88 chroma), not a single NV12 layer. Collecting all planes and
  // setting the combined YUV fourcc makes Mesa sample it as NV12 (YUV->RGB in the sampler), not as
  // disjoint R8/GR88 textures (which drops chroma — the "122,0,0" luma-only bug).
  std::uint32_t pc = 0;
  std::uint64_t total = 0;
  for (int L = 0; L < desc->nb_layers && pc < gfx::DmabufFrame::kMaxPlanes; ++L) {
    const AVDRMLayerDescriptor& layer = desc->layers[L];
    for (int P = 0; P < layer.nb_planes && pc < gfx::DmabufFrame::kMaxPlanes; ++P) {
      f.planes[pc].offset = static_cast<std::uint32_t>(layer.planes[P].offset);
      f.planes[pc].stride = static_cast<std::uint32_t>(layer.planes[P].pitch);
      const std::uint64_t end = f.planes[pc].offset +
                                static_cast<std::uint64_t>(f.planes[pc].stride) * d->height;
      if (end > total) total = end;
      ++pc;
    }
  }
  f.plane_count = pc;
  f.mem_size = total;
  f.host_mapped = false;  // GPU surface — not CPU-mappable
  // The combined fourcc from the plane count: 2 -> NV12, 3 -> YUV420 (8-bit; 10-bit P010/I010 is a
  // later case), 1 -> the single layer's own format (packed RGB). This is what the EGLImage samples as.
  if (pc == 2) f.drm_format = DRM_FORMAT_NV12;
  else if (pc == 3) f.drm_format = DRM_FORMAT_YUV420;
  else f.drm_format = desc->layers[0].format;

  *out = f;
  return 1;
}

}  // namespace acl::media

#endif  // ACL_HAVE_GL

// SPDX-License-Identifier: Apache-2.0
// Provenance: FFmpeg public API (libav*, LGPL, dynamic link only — dlopen'd
// RTLD_LOCAL, per plan.md policy and mirroring src/audio/soundpool.cpp's FFmpeg
// isolation), the VAAPI hwaccel setup (avcodec_get_hw_config +
// AV_HWDEVICE_TYPE_VAAPI + a get_format callback) and DRM_PRIME export
// (av_hwframe_map AV_HWFRAME_MAP_DIRECT -> AVDRMFrameDescriptor,
// hwcontext_drm.h). No GPL-3.0 sources.

#include "media/video_decoder.h"

#if ACL_HAVE_GL

#include <dlfcn.h>
#include <drm_fourcc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#if __has_include(<libavcodec/bsf.h>)
#include <libavcodec/bsf.h>  // its own header since FFmpeg 4.3
#endif
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
}

#include "gfx/sw_frame_pool.h"
#include "log/log.h"
#include "media/codec_engine.h"
#include "media/dma_codec.h"
#include "media/gst_codec.h"

namespace acl::media {
namespace {
constexpr const char* kTag = "acl.media.video";

// ── FFmpeg resolved at runtime (RTLD_LOCAL), like soundpool — keeps its
// transitive deps private.
struct Ff {
#define FF_FN(n) decltype(&::n) n = nullptr
  FF_FN(av_malloc);
  FF_FN(av_strerror);
  FF_FN(av_frame_alloc);
  FF_FN(av_frame_free);
  FF_FN(av_frame_clone);
  FF_FN(av_frame_unref);
  FF_FN(av_packet_alloc);
  FF_FN(av_packet_free);
  FF_FN(av_packet_unref);
  FF_FN(av_buffer_ref);
  FF_FN(av_buffer_unref);
  FF_FN(avformat_open_input);
  FF_FN(avformat_find_stream_info);
  FF_FN(avformat_close_input);
  FF_FN(av_find_best_stream);
  FF_FN(av_read_frame);
  FF_FN(avcodec_find_decoder);
  FF_FN(avcodec_find_decoder_by_name);
  FF_FN(avcodec_alloc_context3);
  FF_FN(avcodec_parameters_to_context);
  FF_FN(avcodec_parameters_copy);
  FF_FN(av_bsf_get_by_name);
  FF_FN(av_bsf_alloc);
  FF_FN(av_bsf_init);
  FF_FN(av_bsf_send_packet);
  FF_FN(av_bsf_receive_packet);
  FF_FN(av_bsf_free);
  FF_FN(avcodec_open2);
  FF_FN(avcodec_free_context);
  FF_FN(avcodec_get_hw_config);
  FF_FN(avcodec_send_packet);
  FF_FN(avcodec_receive_frame);
  FF_FN(avcodec_flush_buffers);
  FF_FN(av_hwdevice_ctx_create);
  FF_FN(av_hwframe_map);
  FF_FN(av_hwdevice_find_type_by_name);
  FF_FN(av_hwdevice_get_type_name);
#undef FF_FN
  bool ok = false;
};
Ff g_ff;
bool g_ff_tried = false;

void* ff_open(const char* base, int maj) {
  char v[64];
  std::snprintf(v, sizeof(v), "%s.%d", base, maj);
  if (void* h = ::dlopen(v, RTLD_NOW | RTLD_LOCAL))
    return h;
  return ::dlopen(base, RTLD_NOW | RTLD_LOCAL);
}

bool load_ffmpeg() {
  if (g_ff_tried)
    return g_ff.ok;
  g_ff_tried = true;
  void* h[3] = {
      ff_open("libavutil.so", LIBAVUTIL_VERSION_MAJOR),
      ff_open("libavcodec.so", LIBAVCODEC_VERSION_MAJOR),
      ff_open("libavformat.so", LIBAVFORMAT_VERSION_MAJOR),
  };
  for (void* x : h)
    if (x == nullptr) {
      log::w(kTag, "FFmpeg dlopen failed; video decode off");
      return false;
    }
  auto sym = [&](const char* n) -> void* {
    for (void* x : h)
      if (void* s = ::dlsym(x, n))
        return s;
    return nullptr;
  };
  bool all = true;
#define FF_BIND(n)                                        \
  do {                                                    \
    g_ff.n = reinterpret_cast<decltype(g_ff.n)>(sym(#n)); \
    if (!g_ff.n) {                                        \
      all = false;                                        \
      log::w(kTag, "FFmpeg missing {}", #n);              \
    }                                                     \
  } while (0)
  FF_BIND(av_malloc);
  FF_BIND(av_strerror);
  FF_BIND(av_frame_alloc);
  FF_BIND(av_frame_free);
  FF_BIND(av_frame_clone);
  FF_BIND(av_frame_unref);
  FF_BIND(av_packet_alloc);
  FF_BIND(av_packet_free);
  FF_BIND(av_packet_unref);
  FF_BIND(av_buffer_ref);
  FF_BIND(av_buffer_unref);
  FF_BIND(avformat_open_input);
  FF_BIND(avformat_find_stream_info);
  FF_BIND(avformat_close_input);
  FF_BIND(av_find_best_stream);
  FF_BIND(av_read_frame);
  FF_BIND(avcodec_find_decoder);
  FF_BIND(avcodec_find_decoder_by_name);
  FF_BIND(avcodec_alloc_context3);
  FF_BIND(avcodec_parameters_to_context);
  FF_BIND(avcodec_parameters_copy);
  FF_BIND(av_bsf_get_by_name);
  FF_BIND(av_bsf_alloc);
  FF_BIND(av_bsf_init);
  FF_BIND(av_bsf_send_packet);
  FF_BIND(av_bsf_receive_packet);
  FF_BIND(av_bsf_free);
  FF_BIND(avcodec_open2);
  FF_BIND(avcodec_free_context);
  FF_BIND(avcodec_get_hw_config);
  FF_BIND(avcodec_send_packet);
  FF_BIND(avcodec_receive_frame);
  FF_BIND(avcodec_flush_buffers);
  FF_BIND(av_hwdevice_ctx_create);
  FF_BIND(av_hwframe_map);
  FF_BIND(av_hwdevice_find_type_by_name);
  FF_BIND(av_hwdevice_get_type_name);
#undef FF_BIND
  g_ff.ok = all;
  log::i(kTag, "FFmpeg video decode {}",
         all ? "ready (RTLD_LOCAL)" : "unavailable");
  return all;
}

// The decoder's negotiated hardware pixel format, stashed in ctx->opaque so
// get_format can return it.
enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                 const enum AVPixelFormat* fmts) {
  const auto want = static_cast<enum AVPixelFormat>(
      reinterpret_cast<std::intptr_t>(ctx->opaque));
  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
    if (*p == want)
      return *p;
  log::w(kTag, "get_format: hw pixfmt not offered; falling back to software");
  return fmts[0];
}

// Shared backend selection (file + packet decoders): attach a HW decode device
// to `ctx`. Preference VAAPI (x86) then DRM (V4L2-request stateless, ARM) via
// avcodec_get_hw_config; ACL_HWDEC_DEVICE overrides the device type. On a
// match, sets ctx->hw_device_ctx + get_format and returns the device in
// *hw_dev. A named vendor M2M decoder (rkmpp/v4l2m2m) may match none — it
// self-produces DRM_PRIME, so leaving no hwdevice is valid. So is a decoder
// with no device at all: its frames are in system memory and are copied
// (sw_to_dmabuf). Always returns true.
bool configure_hw_decode(AVCodecContext* ctx,
                         const AVCodec* codec,
                         AVBufferRef** hw_dev,
                         bool* hw = nullptr) {
  if (hw != nullptr)
    *hw = false;
  enum AVPixelFormat hw_pixfmt = AV_PIX_FMT_NONE;
  enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
  enum AVHWDeviceType prefs[2] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_DRM};
  int nprefs = 2;
  if (const char* dev = std::getenv("ACL_HWDEC_DEVICE")) {
    prefs[0] = g_ff.av_hwdevice_find_type_by_name(dev);
    nprefs = 1;
  }
  for (int pi = 0; pi < nprefs && hw_type == AV_HWDEVICE_TYPE_NONE; ++pi) {
    if (prefs[pi] == AV_HWDEVICE_TYPE_NONE)
      continue;
    for (int i = 0;; ++i) {
      const AVCodecHWConfig* cfg = g_ff.avcodec_get_hw_config(codec, i);
      if (cfg == nullptr)
        break;
      if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
          cfg->device_type == prefs[pi]) {
        hw_pixfmt = cfg->pix_fmt;
        hw_type = prefs[pi];
        break;
      }
    }
  }
  if (hw_type != AV_HWDEVICE_TYPE_NONE) {
    const char* node = std::getenv("ACL_DRM_RENDER_NODE");
    if (g_ff.av_hwdevice_ctx_create(hw_dev, hw_type,
                                    (node && *node) ? node : nullptr, nullptr,
                                    0) < 0) {
      // No such device here (no VAAPI driver, no render node). The decoder
      // still decodes, into system memory; sw_to_dmabuf copies each frame.
      *hw_dev = nullptr;
      log::w(kTag, "no {} device; decoding {} in software",
             g_ff.av_hwdevice_get_type_name(hw_type), codec->name);
      return true;
    }
    ctx->hw_device_ctx = g_ff.av_buffer_ref(*hw_dev);
    ctx->opaque =
        reinterpret_cast<void*>(static_cast<std::intptr_t>(hw_pixfmt));
    ctx->get_format = get_hw_format;
    if (hw != nullptr)
      *hw = true;
    log::i(kTag, "hw decode via {} ({})",
           g_ff.av_hwdevice_get_type_name(hw_type), codec->name);
  } else {
    log::i(kTag, "decode via {} (no hwdevice)", codec->name);
  }
  return true;
}

// Shared DRM_PRIME descriptor -> gfx::DmabufFrame (file + packet decoders).
// Flattens EVERY layer's planes (VAAPI/radeonsi exports NV12 as R8 luma + GR88
// chroma layers, not one NV12 layer — reading only layer0 drops chroma) and
// sets the combined YUV fourcc from the plane count. Single-object only.
int drm_desc_to_dmabuf(const AVDRMFrameDescriptor* desc,
                       std::uint32_t w,
                       std::uint32_t h,
                       gfx::DmabufFrame* out) {
  if (desc == nullptr || desc->nb_objects < 1 || desc->nb_layers < 1)
    return -1;
  if (desc->nb_objects != 1)
    return -2;  // multi-object (per-plane fds) not yet supported
  gfx::DmabufFrame f{};
  f.fd = desc->objects[0].fd;
  f.width = w;
  f.height = h;
  f.modifier = desc->objects[0].format_modifier;
  f.modifier_tiled = f.modifier != 0 && f.modifier != DRM_FORMAT_MOD_INVALID;
  std::uint32_t pc = 0;
  std::uint64_t total = 0;
  for (int L = 0; L < desc->nb_layers && pc < gfx::DmabufFrame::kMaxPlanes;
       ++L) {
    const AVDRMLayerDescriptor& layer = desc->layers[L];
    for (int P = 0; P < layer.nb_planes && pc < gfx::DmabufFrame::kMaxPlanes;
         ++P) {
      f.planes[pc].offset = static_cast<std::uint32_t>(layer.planes[P].offset);
      f.planes[pc].stride = static_cast<std::uint32_t>(layer.planes[P].pitch);
      const std::uint64_t end =
          f.planes[pc].offset +
          static_cast<std::uint64_t>(f.planes[pc].stride) * h;
      if (end > total)
        total = end;
      ++pc;
    }
  }
  f.plane_count = pc;
  f.mem_size = total;
  f.host_mapped = false;
  if (pc == 2)
    f.drm_format = DRM_FORMAT_NV12;
  else if (pc == 3)
    f.drm_format = DRM_FORMAT_YUV420;
  else
    f.drm_format = desc->layers[0].format;
  *out = f;
  return 1;
}

// A DRM_PRIME descriptor for a decoded frame: a hwaccel surface (VAAPI/DRM)
// mapped DIRECT (zero-copy export, no download), or a vendor M2M decoder's
// already-DRM_PRIME frame used as-is. nullptr on fail.
const AVDRMFrameDescriptor* map_drm(AVFrame* hw, AVFrame* drm) {
  if (hw->format == AV_PIX_FMT_DRM_PRIME)
    return reinterpret_cast<const AVDRMFrameDescriptor*>(hw->data[0]);
  drm->format = AV_PIX_FMT_DRM_PRIME;
  if (g_ff.av_hwframe_map(drm, hw,
                          AV_HWFRAME_MAP_DIRECT | AV_HWFRAME_MAP_READ) < 0)
    return nullptr;
  return reinterpret_cast<const AVDRMFrameDescriptor*>(drm->data[0]);
}

// True for a frame the decoder left in system memory: a software decoder, or
// a hardware one that hands back mapped buffers (FFmpeg's V4L2 M2M wrapper).
bool is_sw_frame(const AVFrame* f) {
  return f->format != AV_PIX_FMT_DRM_PRIME && f->hw_frames_ctx == nullptr;
}

// Copies a system-memory frame into an NV12 dma-buf from *pool (made on first
// use, and again when the picture size changes), so it goes down the same path
// as a hardware surface at the cost of one copy. 8-bit 4:2:0 only.
bool sw_to_dmabuf(const AVFrame* f,
                  std::shared_ptr<gfx::SwFramePool>* pool,
                  gfx::DmabufFrame* out,
                  std::string* error) {
  const bool planar =
      f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
  if (!planar && f->format != AV_PIX_FMT_NV12) {
    if (error)
      *error = "software frame is not 8-bit 4:2:0 (pixel format " +
               std::to_string(f->format) + ")";
    return false;
  }
  if (f->width <= 0 || f->height <= 0 || f->data[0] == nullptr ||
      f->data[1] == nullptr || (planar && f->data[2] == nullptr) ||
      f->linesize[0] < f->width) {
    if (error)
      *error = "malformed software frame";
    return false;
  }
  const auto w = static_cast<std::uint32_t>(f->width);
  const auto h = static_cast<std::uint32_t>(f->height);
  if (*pool == nullptr || (*pool)->width() != w || (*pool)->height() != h) {
    *pool = gfx::SwFramePool::create(w, h, error);
    if (*pool == nullptr)
      return false;
    log::i(kTag, "software frames {}x{}: copied to NV12 dma-bufs", w, h);
  }
  const std::uint8_t* const planes[3] = {f->data[0], f->data[1],
                                         planar ? f->data[2] : nullptr};
  const int strides[3] = {f->linesize[0], f->linesize[1],
                          planar ? f->linesize[2] : 0};
  return (*pool)->write_420(planes, strides, !planar, out, error);
}

// MediaCodec MIME -> AVCodecID (the video codecs acl targets). AV_CODEC_ID_NONE
// if unmapped.
enum AVCodecID codec_id_for_mime(const std::string& mime) {
  if (mime == "video/avc")
    return AV_CODEC_ID_H264;
  if (mime == "video/hevc")
    return AV_CODEC_ID_HEVC;
  if (mime == "video/x-vnd.on2.vp8")
    return AV_CODEC_ID_VP8;
  if (mime == "video/x-vnd.on2.vp9")
    return AV_CODEC_ID_VP9;
  if (mime == "video/av01")
    return AV_CODEC_ID_AV1;
  if (mime == "video/mp4v-es")
    return AV_CODEC_ID_MPEG4;
  if (mime == "video/3gpp")
    return AV_CODEC_ID_H263;
  if (mime == "video/mpeg2")
    return AV_CODEC_ID_MPEG2VIDEO;
  return AV_CODEC_ID_NONE;
}
}  // namespace

struct VideoDecoder::Impl {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  AVBufferRef* hw_device = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* hw_frame = nullptr;   // the decoded VAAPI surface
  AVFrame* drm_frame = nullptr;  // hw_frame mapped to DRM_PRIME (its descriptor
                                 // owns the dma-buf fds)
  int stream_index = -1;
  std::uint32_t width = 0, height = 0;
  std::shared_ptr<gfx::SwFramePool> sw_pool;  // software frames only

  ~Impl() {
    if (drm_frame)
      g_ff.av_frame_free(&drm_frame);
    if (hw_frame)
      g_ff.av_frame_free(&hw_frame);
    if (pkt)
      g_ff.av_packet_free(&pkt);
    if (codec)
      g_ff.avcodec_free_context(&codec);
    if (hw_device)
      g_ff.av_buffer_unref(&hw_device);
    if (fmt)
      g_ff.avformat_close_input(&fmt);
  }
};

VideoDecoder::VideoDecoder() : d_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;
std::uint32_t VideoDecoder::width() const {
  return d_->width;
}
std::uint32_t VideoDecoder::height() const {
  return d_->height;
}

std::unique_ptr<VideoDecoder> VideoDecoder::open(const std::string& path,
                                                 std::string* error) {
  auto fail = [&](const char* m) -> std::unique_ptr<VideoDecoder> {
    if (error)
      *error = m;
    return nullptr;
  };
  if (!load_ffmpeg())
    return fail("FFmpeg unavailable");

  auto dec = std::unique_ptr<VideoDecoder>(new VideoDecoder());
  Impl* d = dec->d_.get();

  if (g_ff.avformat_open_input(&d->fmt, path.c_str(), nullptr, nullptr) < 0)
    return fail("open_input failed");
  if (g_ff.avformat_find_stream_info(d->fmt, nullptr) < 0)
    return fail("find_stream_info failed");

#if LIBAVFORMAT_VERSION_MAJOR >= 59
  const AVCodec* codec = nullptr;
  d->stream_index =
      g_ff.av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
#else  // FFmpeg 4: the decoder comes back through a non-const pointer
  AVCodec* found = nullptr;
  d->stream_index =
      g_ff.av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &found, 0);
  const AVCodec* codec = found;
#endif
  if (d->stream_index < 0 || codec == nullptr)
    return fail("no video stream");

  // Backend selection — the zero-copy DRM_PRIME output + import is IDENTICAL
  // across all; only the device/driver differs. ACL_VIDEO_DECODER forces a
  // named decoder (e.g. h264_v4l2m2m / h264_rkmpp on ARM/automotive boards —
  // the V4L2 M2M vendor decoders, which use the M2M hardware and output
  // DRM_PRIME directly, needing no hwdevice). Otherwise the default decoder + a
  // HW device: VAAPI on x86 desktop, or DRM (V4L2-request stateless — RK3588
  // rkvdec etc.) on ARM.
  if (const char* named = std::getenv("ACL_VIDEO_DECODER")) {
    if (const AVCodec* nc = g_ff.avcodec_find_decoder_by_name(named))
      codec = nc;
    else
      log::w(kTag, "ACL_VIDEO_DECODER='{}' not found; using default {}", named,
             codec->name);
  }

  d->codec = g_ff.avcodec_alloc_context3(codec);
  if (d->codec == nullptr)
    return fail("alloc codec context failed");
  if (g_ff.avcodec_parameters_to_context(
          d->codec, d->fmt->streams[d->stream_index]->codecpar) < 0)
    return fail("parameters_to_context failed");
  if (!configure_hw_decode(d->codec, codec, &d->hw_device))
    return fail("av_hwdevice_ctx_create failed");
  if (g_ff.avcodec_open2(d->codec, codec, nullptr) < 0)
    return fail("avcodec_open2 failed");
  d->width = static_cast<std::uint32_t>(d->codec->width);
  d->height = static_cast<std::uint32_t>(d->codec->height);

  d->pkt = g_ff.av_packet_alloc();
  d->hw_frame = g_ff.av_frame_alloc();
  d->drm_frame = g_ff.av_frame_alloc();
  if (!d->pkt || !d->hw_frame || !d->drm_frame)
    return fail("frame/packet alloc failed");

  log::i(kTag, "opened {}x{} VAAPI decode (stream {})", d->width, d->height,
         d->stream_index);
  return dec;
}

int VideoDecoder::next_frame(gfx::DmabufFrame* out, std::string* error) {
  auto err = [&](const char* m) -> int {
    if (error)
      *error = m;
    return -1;
  };
  if (out == nullptr)
    return err("null out");
  Impl* d = d_.get();

  // Release the previous frame's DRM mapping (its dma-buf fds) before decoding
  // the next.
  g_ff.av_frame_unref(d->drm_frame);
  g_ff.av_frame_unref(d->hw_frame);

  for (;;) {
    int r = g_ff.avcodec_receive_frame(d->codec, d->hw_frame);
    if (r == 0)
      break;  // got a decoded surface
    if (r == AVERROR_EOF)
      return 0;  // end of stream
    if (r != AVERROR(EAGAIN))
      return err("avcodec_receive_frame failed");
    // Need more input: read + send the next video packet (or drain at EOF).
    int rr = g_ff.av_read_frame(d->fmt, d->pkt);
    if (rr == AVERROR_EOF) {
      g_ff.avcodec_send_packet(d->codec, nullptr);
      continue;
    }  // flush
    if (rr < 0)
      return err("av_read_frame failed");
    if (d->pkt->stream_index == d->stream_index) {
      const int sr = g_ff.avcodec_send_packet(d->codec, d->pkt);
      g_ff.av_packet_unref(d->pkt);
      if (sr < 0 && sr != AVERROR(EAGAIN))
        return err("avcodec_send_packet failed");
    } else {
      g_ff.av_packet_unref(d->pkt);
    }
  }

  if (is_sw_frame(d->hw_frame))
    return sw_to_dmabuf(d->hw_frame, &d->sw_pool, out, error) ? 1 : -1;
  const AVDRMFrameDescriptor* desc = map_drm(d->hw_frame, d->drm_frame);
  if (desc == nullptr)
    return err(
        "av_hwframe_map(DRM_PRIME) failed (decoder not outputting a hw "
        "surface?)");
  const int rc = drm_desc_to_dmabuf(desc, d->width, d->height, out);
  if (rc == -2)
    return err("multi-object DRM frame (per-plane fds) not yet supported");
  if (rc != 1)
    return err("empty/invalid DRM descriptor");
  return 1;
}

// ── Demuxer (MediaExtractor's role)
// ───────────────────────────────────────────────────────────────
namespace {
const char* mime_for_codec_id(enum AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264:
      return "video/avc";
    case AV_CODEC_ID_HEVC:
      return "video/hevc";
    case AV_CODEC_ID_VP8:
      return "video/x-vnd.on2.vp8";
    case AV_CODEC_ID_VP9:
      return "video/x-vnd.on2.vp9";
    case AV_CODEC_ID_AV1:
      return "video/av01";
    case AV_CODEC_ID_MPEG4:
      return "video/mp4v-es";
    case AV_CODEC_ID_MPEG2VIDEO:
      return "video/mpeg2";
    default:
      return "";
  }
}
}  // namespace

struct Demuxer::DImpl {
  AVFormatContext* fmt = nullptr;
  AVPacket* pkt = nullptr;
  // H.264 and HEVC out of a container that keeps them length-prefixed (MP4,
  // Matroska) are rewritten to Annex B, parameter sets included: what
  // MediaExtractor hands an app and what a MediaCodec engine is given.
  // nullptr for any other codec.
  AVBSFContext* bsf = nullptr;
  int stream_index = -1;
  std::string mime;
  std::vector<std::uint8_t> extradata;
  int width = 0, height = 0;
  ~DImpl() {
    if (bsf)
      g_ff.av_bsf_free(&bsf);
    if (pkt)
      g_ff.av_packet_free(&pkt);
    if (fmt)
      g_ff.avformat_close_input(&fmt);
  }
};

Demuxer::Demuxer() : d_(std::make_unique<DImpl>()) {}
Demuxer::~Demuxer() = default;
const std::string& Demuxer::mime() const {
  return d_->mime;
}
const std::uint8_t* Demuxer::extradata() const {
  return d_->extradata.empty() ? nullptr : d_->extradata.data();
}
std::size_t Demuxer::extradata_size() const {
  return d_->extradata.size();
}
int Demuxer::width() const {
  return d_->width;
}
int Demuxer::height() const {
  return d_->height;
}

std::unique_ptr<Demuxer> Demuxer::open(const std::string& path,
                                       std::string* error) {
  auto fail = [&](const char* m) -> std::unique_ptr<Demuxer> {
    if (error)
      *error = m;
    return nullptr;
  };
  if (!load_ffmpeg())
    return fail("FFmpeg unavailable");
  auto dm = std::unique_ptr<Demuxer>(new Demuxer());
  DImpl* d = dm->d_.get();
  if (g_ff.avformat_open_input(&d->fmt, path.c_str(), nullptr, nullptr) < 0)
    return fail("open_input failed");
  if (g_ff.avformat_find_stream_info(d->fmt, nullptr) < 0)
    return fail("find_stream_info failed");
  d->stream_index =
      g_ff.av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (d->stream_index < 0)
    return fail("no video stream");
  const AVCodecParameters* par = d->fmt->streams[d->stream_index]->codecpar;
  d->width = par->width;
  d->height = par->height;
  d->mime = mime_for_codec_id(par->codec_id);
  const char* to_annexb = par->codec_id == AV_CODEC_ID_H264 ? "h264_mp4toannexb"
                          : par->codec_id == AV_CODEC_ID_HEVC
                              ? "hevc_mp4toannexb"
                              : nullptr;
  if (to_annexb != nullptr) {
    // The filter passes a stream that is Annex B already through as it is.
    const AVBitStreamFilter* f = g_ff.av_bsf_get_by_name(to_annexb);
    if (f == nullptr || g_ff.av_bsf_alloc(f, &d->bsf) < 0)
      return fail("no Annex B bitstream filter");
    d->bsf->time_base_in = d->fmt->streams[d->stream_index]->time_base;
    if (g_ff.avcodec_parameters_copy(d->bsf->par_in, par) < 0 ||
        g_ff.av_bsf_init(d->bsf) < 0)
      return fail("Annex B bitstream filter did not start");
    par = d->bsf->par_out;  // its extradata is the parameter sets, Annex B
  }
  if (par->extradata != nullptr && par->extradata_size > 0)
    d->extradata.assign(par->extradata, par->extradata + par->extradata_size);
  d->pkt = g_ff.av_packet_alloc();
  if (!d->pkt)
    return fail("packet alloc failed");
  log::i(kTag, "demux {}x{} {} (extradata {}B)", d->width, d->height,
         d->mime.empty() ? "?" : d->mime.c_str(), d->extradata.size());
  return dm;
}

int Demuxer::read_sample(const std::uint8_t** data,
                         std::size_t* size,
                         std::int64_t* pts_us,
                         std::string* error) {
  DImpl* d = d_.get();
  g_ff.av_packet_unref(
      d->pkt);  // release the previous sample before overwriting it
  for (;;) {
    int r = g_ff.av_read_frame(d->fmt, d->pkt);
    if (r == AVERROR_EOF)
      return 0;
    if (r < 0) {
      if (error)
        *error = "av_read_frame failed";
      return -1;
    }
    if (d->pkt->stream_index == d->stream_index) {
      if (d->bsf != nullptr) {
        // Takes the packet; gives it back rewritten, one for one.
        if (g_ff.av_bsf_send_packet(d->bsf, d->pkt) < 0) {
          g_ff.av_packet_unref(d->pkt);
          if (error)
            *error = "Annex B bitstream filter refused a packet";
          return -1;
        }
        r = g_ff.av_bsf_receive_packet(d->bsf, d->pkt);
        if (r == AVERROR(EAGAIN))
          continue;
        if (r < 0) {
          if (error)
            *error = "Annex B bitstream filter failed";
          return -1;
        }
      }
      if (data)
        *data = d->pkt->data;
      if (size)
        *size = static_cast<std::size_t>(d->pkt->size);
      if (pts_us)
        *pts_us = d->pkt->pts;  // stream time_base units (MediaExtractor
                                // converts; test ignores)
      return 1;
    }
    g_ff.av_packet_unref(d->pkt);  // not our stream — skip
  }
}

// ── CodecDecoder (MediaCodec's decode engine)
// ─────────────────────────────────────────────────────
struct CodecDecoder::CImpl {
  AVCodecContext* codec = nullptr;
  AVBufferRef* hw_device = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* hw_frame = nullptr;
  AVFrame* drm_frame = nullptr;
  std::uint32_t width = 0, height = 0;
  std::shared_ptr<gfx::SwFramePool> sw_pool;  // software frames only
  // Set: this engine decodes, and nothing above is used.
  std::unique_ptr<CodecEngine> engine;
  ~CImpl() {
    if (drm_frame)
      g_ff.av_frame_free(&drm_frame);
    if (hw_frame)
      g_ff.av_frame_free(&hw_frame);
    if (pkt)
      g_ff.av_packet_free(&pkt);
    if (codec)
      g_ff.avcodec_free_context(
          &codec);  // frees ctx->extradata (av_malloc'd) too
    if (hw_device)
      g_ff.av_buffer_unref(&hw_device);
  }
};

CodecDecoder::CodecDecoder() : d_(std::make_unique<CImpl>()) {}
CodecDecoder::~CodecDecoder() = default;

std::unique_ptr<CodecDecoder> CodecDecoder::configure(const std::string& mime,
                                                      const std::uint8_t* csd,
                                                      std::size_t csd_size,
                                                      int width,
                                                      int height,
                                                      std::string* error) {
  auto fail = [&](const char* m) -> std::unique_ptr<CodecDecoder> {
    if (error)
      *error = m;
    return nullptr;
  };
  // Which engine decodes. FFmpeg with a hardware device (VAAPI, or a named
  // decoder) is used where there is one: that path is zero-copy and the one
  // x86 has run on. Where FFmpeg would decode in software, the device's
  // hardware decoder is tried directly (dmabuf-codec: V4L2, OpenMAX), then
  // through GStreamer, whose installed decoder may be that same hardware at
  // the cost of a copy; FFmpeg in software is what is left.
  // ACL_VIDEO_BACKEND=dma|gst|ffmpeg picks one outright, and the FFmpeg knobs
  // (ACL_VIDEO_DECODER, ACL_HWDEC_DEVICE) mean FFmpeg.
  const char* be = std::getenv("ACL_VIDEO_BACKEND");
  const std::string backend = be != nullptr ? be : "";
  const bool ffmpeg_asked = backend == "ffmpeg" ||
                            std::getenv("ACL_VIDEO_DECODER") != nullptr ||
                            std::getenv("ACL_HWDEC_DEVICE") != nullptr;
  auto dec = std::unique_ptr<CodecDecoder>(new CodecDecoder());
  CImpl* d = dec->d_.get();
  d->width = static_cast<std::uint32_t>(width > 0 ? width : 0);
  d->height = static_cast<std::uint32_t>(height > 0 ? height : 0);
  auto try_dma = [&](std::string* why) {
#if ACL_HAVE_DMABUF_CODEC
    d->engine = DmaCodec::configure(mime, csd, csd_size, width, height, why);
    return d->engine != nullptr;
#else
    if (why)
      *why = "built without dmabuf-codec";
    return false;
#endif
  };
  auto try_gst = [&](std::string* why) {
#if ACL_HAVE_GST
    d->engine = GstCodec::configure(mime, csd, csd_size, why);
    return d->engine != nullptr;
#else
    if (why)
      *why = "built without GStreamer";
    return false;
#endif
  };
  // Where FFmpeg has no hardware to offer and was not asked for by name.
  auto try_others = [&] {
    return !ffmpeg_asked && (try_dma(nullptr) || try_gst(nullptr));
  };
  if (backend == "dma" || backend == "gst") {
    std::string why;
    if (!(backend == "dma" ? try_dma(&why) : try_gst(&why))) {
      if (error)
        *error = why;
      return nullptr;
    }
    return dec;
  }
  if (!load_ffmpeg()) {
    if (try_others())
      return dec;
    return fail("FFmpeg unavailable");
  }
  const enum AVCodecID id = codec_id_for_mime(mime);
  if (id == AV_CODEC_ID_NONE)
    return fail("unsupported MIME");
  const AVCodec* codec = g_ff.avcodec_find_decoder(id);
  if (const char* named = std::getenv("ACL_VIDEO_DECODER")) {
    if (const AVCodec* nc = g_ff.avcodec_find_decoder_by_name(named))
      codec = nc;
  }
  if (codec == nullptr) {
    if (try_others())
      return dec;
    return fail("no decoder for MIME");
  }
  d->codec = g_ff.avcodec_alloc_context3(codec);
  if (d->codec == nullptr)
    return fail("alloc codec context failed");
  d->codec->width = width;
  d->codec->height = height;
  // Codec-specific data (SPS/PPS) as the context's extradata. av_malloc'd (+
  // padding) so avcodec_free_context frees it; the decoder reads it at open
  // time.
  if (csd != nullptr && csd_size > 0) {
    auto* ed = static_cast<std::uint8_t*>(
        g_ff.av_malloc(csd_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (ed == nullptr)
      return fail("extradata alloc failed");
    std::memcpy(ed, csd, csd_size);
    std::memset(ed + csd_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    d->codec->extradata = ed;
    d->codec->extradata_size = static_cast<int>(csd_size);
  }
  bool ff_hw = false;
  if (!configure_hw_decode(d->codec, codec, &d->hw_device, &ff_hw))
    return fail("av_hwdevice_ctx_create failed");
  if (!ff_hw && try_others())
    return dec;  // the unopened FFmpeg context is freed with the decoder
  // Enlarge the HW surface pool: decoded VAAPI surfaces are exported zero-copy
  // and held downstream (the compositor keeps each wl_buffer until
  // wl_buffer.release). With only the decoder's tight default pool, those held
  // surfaces starve decode and it drops frames (~1/3 -> a stuttery 16fps). The
  // extras cover the in-flight plane buffers + a margin. Tunable via
  // ACL_VIDEO_EXTRA_FRAMES.
  d->codec->extra_hw_frames = [] {
    if (const char* e = std::getenv("ACL_VIDEO_EXTRA_FRAMES")) {
      int v = std::atoi(e);
      if (v >= 0)
        return v;
    }
    // A frame's surface is held until the last user of the frame lets go
    // (DmabufFrame::hold): the codec's queued output buffers (8), the
    // images a consumer has acquired, the plane's in-flight buffers (4).
    return 24;
  }();
  if (g_ff.avcodec_open2(d->codec, codec, nullptr) < 0)
    return fail("avcodec_open2 failed");
  d->width = static_cast<std::uint32_t>(width > 0 ? width : d->codec->width);
  d->height =
      static_cast<std::uint32_t>(height > 0 ? height : d->codec->height);
  d->pkt = g_ff.av_packet_alloc();
  d->hw_frame = g_ff.av_frame_alloc();
  d->drm_frame = g_ff.av_frame_alloc();
  if (!d->pkt || !d->hw_frame || !d->drm_frame)
    return fail("frame/packet alloc failed");
  log::i(kTag, "MediaCodec decoder configured {} {}x{} ({})", mime, d->width,
         d->height, codec->name);
  return dec;
}

void CodecDecoder::flush() {
  CImpl* d = d_.get();
  if (d->engine != nullptr)
    return d->engine->flush();
  g_ff.av_frame_unref(d->drm_frame);
  g_ff.av_frame_unref(d->hw_frame);
  g_ff.avcodec_flush_buffers(d->codec);
}

int CodecDecoder::send_packet(const std::uint8_t* data,
                              std::size_t size,
                              std::int64_t pts_us,
                              std::string* error) {
  CImpl* d = d_.get();
  if (d->engine != nullptr)
    return d->engine->send_packet(data, size, pts_us, error);
  if (data == nullptr || size == 0) {  // end-of-stream: drain
    const int r = g_ff.avcodec_send_packet(d->codec, nullptr);
    if (r < 0 && r != AVERROR_EOF) {
      if (error)
        *error = "send EOS failed";
      return -1;
    }
    return 0;
  }
  // Borrow the caller's compressed buffer (small — the bitstream, NOT pixels;
  // the decoder deep-copies it internally since d->pkt->buf is null). Cleared
  // after so d->pkt never frees the caller's memory.
  d->pkt->data = const_cast<std::uint8_t*>(data);
  d->pkt->size = static_cast<int>(size);
  d->pkt->pts = pts_us;
  const int r = g_ff.avcodec_send_packet(d->codec, d->pkt);
  d->pkt->data = nullptr;
  d->pkt->size = 0;
  if (r == AVERROR(EAGAIN))
    return 1;  // output full — caller must receive_frame, then retry this
               // packet
  if (r < 0) {
    if (error) {
      char b[256] = {0};
      g_ff.av_strerror(r, b, sizeof(b));
      *error =
          "avcodec_send_packet failed (" + std::to_string(r) + " " + b + ")";
    }
    return -1;
  }
  return 0;
}

int CodecDecoder::receive_frame(gfx::DmabufFrame* out,
                                std::int64_t* pts_us,
                                std::string* error) {
  auto err = [&](const char* m) -> int {
    if (error)
      *error = m;
    return -1;
  };
  if (out == nullptr)
    return err("null out");
  CImpl* d = d_.get();
  if (d->engine != nullptr)
    return d->engine->receive_frame(out, pts_us, error);
  g_ff.av_frame_unref(d->drm_frame);
  g_ff.av_frame_unref(d->hw_frame);
  const int r = g_ff.avcodec_receive_frame(d->codec, d->hw_frame);
  if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
    return 0;  // none ready / drained
  if (r < 0)
    return err("avcodec_receive_frame failed");
  if (is_sw_frame(d->hw_frame)) {
    // The copy owns its buffer (out->hold); the decoder's frame is not needed
    // past this call.
    if (!sw_to_dmabuf(d->hw_frame, &d->sw_pool, out, error))
      return -1;
    if (pts_us)
      *pts_us = d->hw_frame->pts;
    return 1;
  }
  const AVDRMFrameDescriptor* desc = map_drm(d->hw_frame, d->drm_frame);
  if (desc == nullptr)
    return err("map DRM_PRIME failed (decoder not outputting a hw surface?)");
  const int rc = drm_desc_to_dmabuf(desc, d->width, d->height, out);
  if (rc == -2)
    return err("multi-object DRM frame (per-plane fds) not yet supported");
  if (rc != 1)
    return err("invalid DRM descriptor");
  if (pts_us)
    *pts_us = d->hw_frame->pts;
  // Keep the surface out of the decoder's pool for as long as the frame is
  // in use. Without this the next receive_frame() returns it to the pool,
  // the decoder draws a later picture into it, and a frame still waiting to
  // be shown shows that later picture.
  struct Hold {
    AVFrame* hw = nullptr;
    AVFrame* drm = nullptr;
    ~Hold() {
      if (drm != nullptr)
        g_ff.av_frame_free(&drm);
      if (hw != nullptr)
        g_ff.av_frame_free(&hw);
    }
  };
  auto hold = std::make_shared<Hold>();
  hold->hw = g_ff.av_frame_clone(d->hw_frame);
  hold->drm = g_ff.av_frame_clone(d->drm_frame);
  if (hold->hw == nullptr)
    return err("out of memory holding the decoded frame");
  out->hold = std::move(hold);
  return 1;
}

}  // namespace acl::media

#endif  // ACL_HAVE_GL

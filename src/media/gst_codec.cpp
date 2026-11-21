// SPDX-License-Identifier: Apache-2.0
// Provenance: GStreamer public API documentation (appsrc, appsink,
// GstElementFactory, GstVideoMeta, gstdmabuf.h). GStreamer is
// LGPL-2.1-or-later, dynamically linked. No GPL-3.0 sources.

#include "media/gst_codec.h"

#if ACL_HAVE_GL && ACL_HAVE_GST

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "gfx/sw_frame_pool.h"
#include "log/log.h"
#include "media/h264_color.h"

#if ACL_HAVE_DMABUF_CODEC
#include "parse/h265/nal.h"  // dmabuf-codec's H.265 NAL and SPS readers
#include "parse/h265/sps.h"
#include "parse/sps_color.h"
#endif

namespace acl::media {
namespace {
constexpr const char* kTag = "acl.media.gst";

// Input the pipeline may hold before send_packet says "receive first". The
// appsink holds kSinkBuffers decoded frames and then blocks the decoder, so
// without this the encoded stream would pile up in appsrc without bound.
constexpr guint64 kMaxQueuedBytes = 2u * 1024u * 1024u;
constexpr guint kSinkBuffers = 4;
// Frames of a decoder whose pool is small and fixed that may be out of it at
// once (see tagged_fd): one fewer than the buffers the decoder has been seen
// to use, so that it always has one to decode into, and this many until more
// have been seen. The board this was found on gives its decoder 9 output
// buffers whatever the consumer asks for. A frame beyond the limit is copied,
// which gives its buffer straight back.
constexpr std::size_t kTaggedOutMin = 4;
// Access units the pipeline may hold that have not come out as frames. A
// MediaCodec takes input only about as fast as it gives output, and a player
// counts on that: what is inside the codec is what a seek throws away. The
// allowance starts small and grows only when the decoder shows it needs more
// before it will emit anything (frame reordering).
constexpr std::uint64_t kInFlightStart = 4;
constexpr std::uint64_t kInFlightMax = 32;
constexpr gint64 kStarvedUs = 50 * 1000;

// ACL_VIDEO_GST_COPY: take every frame through system memory, to try the copy
// path on a machine whose decoder has dma-bufs to give.
bool copy_always() {
  static const bool v = std::getenv("ACL_VIDEO_GST_COPY") != nullptr;
  return v;
}

// The appsink's answer to a decoder asking what the consumer supports: plane
// offsets and strides carried on the buffer (GstVideoMeta). A decoder gives
// out its own buffers only to a consumer that says so: VA will not negotiate
// dma-bufs at all without it. An appsink says nothing by itself, and the
// callback that lets it is from GStreamer 1.24, so the query is answered on
// the pad, which every version has.
GstPadProbeReturn answer_allocation(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
  if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION)
    return GST_PAD_PROBE_OK;
  gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
  return GST_PAD_PROBE_HANDLED;  // answered, and the answer is yes
}

// The picture's matrix and range, onto the frame. Where the stream itself
// said them (`said`, from an H.264 or HEVC SPS) its word is taken: a
// decoder's caps may carry a default instead. GStreamer 1.16's h264parse and
// h265parse give no colorimetry, and omxh264dec and omxh265dec then state
// bt601 (limited range) whatever the stream says. Otherwise the caps, with the
// matrix as H.273 numbers it; GStreamer's own function for that is from 1.18,
// and this runs on 1.16.
void note_color(const GstVideoInfo& info,
                const H264Color* said,
                gfx::DmabufFrame* out) {
  if (said != nullptr) {
    out->yuv_matrix = said->matrix;
    out->yuv_full_range = said->full_range;
    return;
  }
  std::uint8_t matrix = 2;  // not said
  switch (info.colorimetry.matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT709:
      matrix = 1;
      break;
    case GST_VIDEO_COLOR_MATRIX_BT601:
      matrix = 6;
      break;
    case GST_VIDEO_COLOR_MATRIX_BT2020:
      matrix = 9;
      break;
    default:
      break;
  }
  out->yuv_matrix = matrix;
  out->yuv_full_range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
}

// Qualcomm's OpenMAX decoder element hands out system memory that is a
// mapping of the decoder's own buffer, and names the buffer's dma-buf in the
// slots of the GstVideoMeta that a two-plane frame leaves unused: offset[2]
// holds the tag "QaUT" and stride[2] the descriptor. (Its own log says so:
// "Add ion/gbm fd N ... with signature QaUT in GstVideoMeta".) Without this
// every frame of that decoder is copied. The descriptor, or -1 where the meta
// carries no such tag.
int tagged_fd(const GstVideoMeta* vm) {
  constexpr gsize kTag4 = 0x54556151;  // 'Q' 'a' 'U' 'T', little-endian
  if (vm == nullptr || vm->n_planes != 2 || vm->offset[2] != kTag4 ||
      vm->stride[2] < 0)
    return -1;
  return vm->stride[2];
}

#if GST_CHECK_VERSION(1, 24, 0)
// DRM_FORMAT_MOD_BROADCOM_SAND128 with no column height in it, and P030
// (10 bits, three samples in 32), which an older drm_fourcc.h lacks.
constexpr std::uint64_t kSand128 = 0x0700000000000004ull;
constexpr std::uint32_t kP030 = 0x30333050u;  // 'P030'

// The height in rows of one column of plane `i` of a frame in Broadcom's
// columns, from the tiled stride GStreamer gives it: (rows of tiles << 16) |
// columns. A tile is 8 rows of luma or 4 of chroma, at either depth; a Pi 5's
// buffers are that size (320x240: 92160 and 46080 bytes at 8 bits, 122880
// and 61440 at 10). 0 when the stride says nothing usable.
guint sand_rows(guint i, gint stride) {
  const guint tile_h = i == 0 ? 8 : 4;
  if (stride <= 0 || GST_VIDEO_TILE_X_TILES(stride) == 0)
    return 0;
  const guint y_tiles = GST_VIDEO_TILE_Y_TILES(stride);
  if (y_tiles == 0 || y_tiles > 16384 / tile_h)
    return 0;
  return y_tiles * tile_h;
}

// A frame whose caps name a DRM format and modifier (GStreamer 1.24's
// "DMA_DRM"): a hardware decoder's own surface in the layout its driver
// uses, which may be tiled. Each plane is found in the buffer's memories by
// its video meta offset; the planes may be in one dma-buf or one each.
// 1 with *out filled (fds borrowed from the buffer), -1 with *why.
int drm_frame(GstBuffer* buf,
              const GstCaps* caps,
              const H264Color* said,
              gfx::DmabufFrame* out,
              std::string* why) {
  auto bad = [&](const char* m) {
    *why = m;
    return -1;
  };
  GstVideoInfoDmaDrm drm;
  gst_video_info_dma_drm_init(&drm);
  if (!gst_video_info_dma_drm_from_caps(&drm, caps))
    return bad("GStreamer: DRM caps do not parse");
  // What the video plane and the converter take; anything else has no path.
  // P030 only in Broadcom's columns, where a Pi 5's HEVC decoder puts 10 bits.
  const bool sand = drm.drm_modifier == kSand128;
  if (drm.drm_fourcc != DRM_FORMAT_NV12 && drm.drm_fourcc != DRM_FORMAT_P010 &&
      !(sand && drm.drm_fourcc == kP030))
    return bad("GStreamer: DRM frame is not NV12 or P010");
  const int w = GST_VIDEO_INFO_WIDTH(&drm.vinfo);
  const int h = GST_VIDEO_INFO_HEIGHT(&drm.vinfo);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
    return bad("GStreamer: bad frame size");
  const GstVideoMeta* vm = gst_buffer_get_video_meta(buf);
  if (vm == nullptr || vm->n_planes < 2 ||
      vm->n_planes > static_cast<guint>(gfx::DmabufFrame::kMaxPlanes))
    return bad("GStreamer: DRM frame without a plane layout");

  gfx::DmabufFrame f{};
  f.width = static_cast<std::uint32_t>(w);
  f.height = static_cast<std::uint32_t>(h);
  f.drm_format = drm.drm_fourcc;
  f.modifier = drm.drm_modifier;
  f.modifier_tiled = f.modifier != DRM_FORMAT_MOD_LINEAR &&
                     f.modifier != DRM_FORMAT_MOD_INVALID;
  f.plane_count = vm->n_planes;
  const std::uint64_t sample = drm.drm_fourcc == DRM_FORMAT_P010 ? 2 : 1;
  for (guint i = 0; i < vm->n_planes; ++i) {
    guint idx = 0;
    guint len = 0;
    gsize skip = 0;
    if (!gst_buffer_find_memory(buf, vm->offset[i], 1, &idx, &len, &skip))
      return bad("GStreamer: a plane is outside the buffer");
    GstMemory* mem = gst_buffer_peek_memory(buf, idx);
    if (!gst_is_dmabuf_memory(mem))
      return bad("GStreamer: DRM frame is not in dma-bufs");
    const int fd = gst_dmabuf_memory_get_fd(mem);
    gsize mem_off = 0;
    gsize mem_max = 0;
    gst_memory_get_sizes(mem, &mem_off, &mem_max);
    const std::uint64_t off = static_cast<std::uint64_t>(skip) + mem_off;
    const off_t end = fd >= 0 ? ::lseek(fd, 0, SEEK_END) : -1;
    const std::uint64_t size = end > 0 ? static_cast<std::uint64_t>(end) : 0;
    if (fd < 0 || vm->stride[i] <= 0 || off > 0xffffffffu ||
        (size != 0 && off >= size))
      return bad("GStreamer: a plane does not fit its dma-buf");
    // Rows are checked where the layout says what a row is: a tiled
    // modifier's stride is the driver's own business.
    if (!f.modifier_tiled && i < 2) {
      const std::uint64_t row =
          (i == 0 ? static_cast<std::uint64_t>(w)
                  : (static_cast<std::uint64_t>(w) + 1) / 2 * 2) *
          sample;
      const std::uint64_t rows = i == 0
                                     ? static_cast<std::uint64_t>(h)
                                     : (static_cast<std::uint64_t>(h) + 1) / 2;
      const auto stride = static_cast<std::uint64_t>(vm->stride[i]);
      if (stride < row || (size != 0 && off + stride * rows > size))
        return bad("GStreamer: a plane does not fit its dma-buf");
    }
    f.planes[i].offset = static_cast<std::uint32_t>(off);
    f.planes[i].stride = static_cast<std::uint32_t>(vm->stride[i]);
    // Broadcom's columns: GStreamer gives the stride in tiles, (rows of
    // tiles << 16) | columns; the converter and the video plane take the
    // height of a column in rows.
    if (sand) {
      const guint rows = sand_rows(i, vm->stride[i]);
      if (rows == 0 ||
          (size != 0 && off + static_cast<std::uint64_t>(rows) * 128 *
                                    GST_VIDEO_TILE_X_TILES(vm->stride[i]) >
                            size))
        return bad("GStreamer: a plane does not fit its dma-buf");
      f.planes[i].stride = rows;
    }
    if (i == 0) {
      f.fd = fd;
      f.mem_size = size;
    } else if (fd != f.fd) {
      f.planes[i].fd = fd;
    }
  }
  note_color(drm.vinfo, said, &f);
  *out = f;
  return 1;
}
#endif

bool gst_ready() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    GError* e = nullptr;
    ok = gst_init_check(nullptr, nullptr, &e) != FALSE;
    if (!ok)
      log::w(kTag, "gst_init failed: {}", e != nullptr ? e->message : "?");
    g_clear_error(&e);
  });
  return ok;
}

// MediaCodec MIME -> the caps of the elementary stream an app feeds. nullptr
// if unmapped.
const char* caps_for_mime(const std::string& mime) {
  if (mime == "video/avc")
    return "video/x-h264,stream-format=byte-stream";
  if (mime == "video/hevc")
    return "video/x-h265,stream-format=byte-stream";
  if (mime == "video/x-vnd.on2.vp8")
    return "video/x-vp8";
  if (mime == "video/x-vnd.on2.vp9")
    return "video/x-vp9";
  if (mime == "video/av01")
    return "video/x-av1";
  if (mime == "video/mp4v-es")
    return "video/mpeg,mpegversion=4,systemstream=false";
  if (mime == "video/mpeg2")
    return "video/mpeg,mpegversion=2,systemstream=false";
  if (mime == "video/3gpp")
    return "video/x-h263";
  return nullptr;
}

// A software decoder that decodes frames on a thread each holds back one
// frame per thread, which on a many-core machine is a second of video inside
// the codec. Where the element has a thread limit (the libav decoders'
// "max-threads"), keep it small. Hardware decoders have no such property.
void limit_threads(GstElement* e) {
  GParamSpec* ps =
      g_object_class_find_property(G_OBJECT_GET_CLASS(e), "max-threads");
  if (ps != nullptr && G_PARAM_SPEC_VALUE_TYPE(ps) == G_TYPE_INT &&
      (ps->flags & G_PARAM_WRITABLE) != 0)
    g_object_set(e, "max-threads", 4, nullptr);
}

// Whether an HEVC stream is deeper than 8 bits, as the SPS in its csd says.
// False where that cannot be told (no csd, or a4l built without
// dmabuf-codec, whose reader this is).
bool hevc_deep(const std::uint8_t* csd, std::size_t size) {
#if ACL_HAVE_DMABUF_CODEC
  if (csd == nullptr || size == 0)
    return false;
  for (const auto& n : v4l2wc::h265::ParseAnnexB(csd, size)) {
    v4l2wc::h265::Sps sps;
    if (n.type == v4l2wc::h265::NalUnitType::kSpsNut &&
        v4l2wc::h265::ParseSps(n.rbsp.data(), n.rbsp.size(), &sps))
      return sps.bit_depth_luma > 8 || sps.bit_depth_chroma > 8;
  }
#else
  (void)csd;
  (void)size;
#endif
  return false;
}

// GStreamer's name for an H.264 profile beyond 8-bit 4:2:0 (as its
// codec-utils names them), and nullptr for one within it, where the caps are
// left as they were.
const char* h264_profile_past_8bit_420(const H264Format& f) {
  const bool intra = (f.constraint_flags & 0x10) != 0;  // constraint_set3
  switch (f.profile_idc) {
    case 110:
      return intra ? "high-10-intra" : "high-10";
    case 122:
      return intra ? "high-4:2:2-intra" : "high-4:2:2";
    case 244:
      return intra ? "high-4:4:4-intra" : "high-4:4:4";
    case 44:
      return "cavlc-4:4:4-intra";
    default:
      return nullptr;
  }
}

// Whether a decoder's output can be a picture deeper than 8 bits in a form
// taken here: P010 or planar 16-bit, or a dma-buf with its DRM format named.
// Qualcomm's omxh265dec cannot (NV12, or its compressed tiles), and with a
// 10-bit stream fails once data flows ("Could not configure supporting
// library"); a decoder further down the list is wanted instead.
bool gives_deep(GstElementFactory* f) {
  GstCaps* deep = gst_caps_from_string(
      "video/x-raw,format=(string){P010_10LE,I420_10LE}; "
      "video/x-raw(memory:DMABuf),format=(string)DMA_DRM");
  bool yes = false;
  for (const GList* l = gst_element_factory_get_static_pad_templates(f);
       l != nullptr && !yes; l = l->next) {
    auto* t = static_cast<GstStaticPadTemplate*>(l->data);
    if (t->direction != GST_PAD_SRC)
      continue;
    GstCaps* c = gst_static_pad_template_get_caps(t);
    yes = gst_caps_can_intersect(c, deep);
    gst_caps_unref(c);
  }
  gst_caps_unref(deep);
  return yes;
}

// The highest-ranked installed element of `type` that takes `caps`, as an
// autoplugger would choose, and for a `deep` stream one that gives_deep().
// nullptr if there is none. *name gets its factory name, with "(hardware)"
// when it says it is one.
GstElement* pick(GstElementFactoryListType type,
                 const GstCaps* caps,
                 std::string* name,
                 bool deep = false) {
  GList* all = gst_element_factory_list_get_elements(type, GST_RANK_MARGINAL);
  GList* fit = gst_element_factory_list_filter(all, caps, GST_PAD_SINK, FALSE);
  GstElement* e = nullptr;
  for (GList* l = fit; l != nullptr && e == nullptr; l = l->next) {
    auto* f = GST_ELEMENT_FACTORY(l->data);
    if (deep && !gives_deep(f)) {
      log::i(kTag, "{} passed over: no output deeper than 8 bits taken here",
             GST_OBJECT_NAME(f));
      continue;
    }
    e = gst_element_factory_create(f, nullptr);
    if (e != nullptr && name != nullptr) {
      *name = GST_OBJECT_NAME(f);
      const char* klass =
          gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
      if (klass != nullptr && std::strstr(klass, "Hardware") != nullptr)
        *name += " (hardware)";
    }
  }
  gst_plugin_feature_list_free(fit);
  gst_plugin_feature_list_free(all);
  return e;
}
}  // namespace

bool gst_start() {
  return gst_ready();
}

struct GstCodec::Impl {
  GstElement* pipeline = nullptr;
  GstElement* src = nullptr;   // appsrc
  GstElement* sink = nullptr;  // appsink
  GstBus* bus = nullptr;
  std::string csd;  // sent ahead of the first access unit
  // The color the stream's last SPS said, which goes before the caps. Read
  // as the access units go in; one thread makes every call here. A picture
  // still in the decoder when a new SPS arrives is given the new one's color.
  // H.264 with a4l's own reader; HEVC with dmabuf-codec's, where a4l is
  // built with it, and otherwise not at all (the caps alone).
  bool avc = false;
  bool hevc = false;
  bool color_said = false;
  H264Color color;
  const H264Color* said() const { return color_said ? &color : nullptr; }
  void read_color(const std::uint8_t* p, std::size_t n) {
    if (avc && h264_color(p, n, &color))
      color_said = true;
#if ACL_HAVE_DMABUF_CODEC
    v4l2wc::SpsColor c;
    // An SPS with no VUI, or a VUI without the video signal type, reads as
    // matrix 2, limited range: nothing said, and the caps stand.
    if (hevc && p != nullptr &&
        v4l2wc::FindSpsColor(v4l2wc::SpsCodec::kH265, p, n, &c) &&
        (c.matrix_coeffs != 2 || c.video_full_range_flag)) {
      color.matrix = c.matrix_coeffs;
      color.full_range = c.video_full_range_flag;
      color_said = true;
    }
#endif
  }
  bool csd_sent = false;
  bool eos_sent = false;
  std::uint64_t sent = 0;  // access units in
  std::uint64_t got = 0;   // frames out
  std::uint64_t in_flight_max = kInFlightStart;
  gint64 last_progress_us = 0;  // monotonic: last frame out, or allowance raise
  std::shared_ptr<gfx::SwFramePool> sw_pool;
  // Frames passed on by tagged_fd that are still in use. Shared with each
  // such frame, which may outlive the codec.
  std::shared_ptr<std::atomic<int>> tagged_out =
      std::make_shared<std::atomic<int>>(0);
  // The descriptors such frames have come with, for one picture size: the
  // part of the decoder's pool seen so far. A new size is a new pool.
  std::vector<int> tagged_fds;
  int tagged_w = 0;
  int tagged_h = 0;
  std::uint64_t tagged_passed = 0;  // for the tally below
  std::uint64_t tagged_copied = 0;

  // How many frames named by tagged_fd may be out at once, having now seen
  // `fd` on a `w` x `h` frame.
  int tagged_limit(int fd, int w, int h) {
    constexpr std::size_t kMostBuffers = 64;  // more is not a small pool
    if (w != tagged_w || h != tagged_h) {
      tagged_fds.clear();
      tagged_w = w;
      tagged_h = h;
    }
    if (tagged_fds.size() < kMostBuffers &&
        std::find(tagged_fds.begin(), tagged_fds.end(), fd) == tagged_fds.end())
      tagged_fds.push_back(fd);
    return static_cast<int>(std::max(kTaggedOutMin, tagged_fds.size() - 1));
  }

  // Said every so often, since an app seldom ends by releasing its codec:
  // how the frames of such a decoder have gone so far.
  void tally() {
    constexpr std::uint64_t kEvery = 120;
    if ((tagged_passed + tagged_copied) % kEvery == 0)
      log::i(kTag,
             "frames named in the video meta: {} passed on, {} copied (the "
             "decoder has shown {} buffers)",
             tagged_passed, tagged_copied, tagged_fds.size());
  }

  // Runs `step` (a state change, a flush) while taking whatever appsink
  // holds. A decoder may have a drain query waiting in appsink for its queue
  // to empty, and the query keeps the sink pad's stream lock, which the step
  // needs. Stopping the sink unlocks it and undoes the unlock at once; with
  // GStreamer 1.16 (an SA8155P, omxh264dec) the query stayed waiting through
  // that, and the step never returned. Emptying the queue lets the query
  // finish.
  void pumping(const std::function<void()>& step) {
    std::atomic<bool> done{false};
    std::thread t;
    try {
      t = std::thread([&] {
        step();
        done.store(true, std::memory_order_release);
      });
    } catch (const std::system_error&) {
      step();  // no thread to be had: as before
      return;
    }
    while (!done.load(std::memory_order_acquire)) {
      if (GstSample* s =
              gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_MSECOND))
        gst_sample_unref(s);
      else
        g_usleep(1000);  // stopped: pulling returns at once
    }
    t.join();
  }

  ~Impl() {
    if (pipeline != nullptr) {
      if (sink != nullptr)
        pumping([this] { gst_element_set_state(pipeline, GST_STATE_NULL); });
      else
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    if (bus != nullptr)
      gst_object_unref(bus);
    if (src != nullptr)
      gst_object_unref(src);
    if (sink != nullptr)
      gst_object_unref(sink);
    if (pipeline != nullptr)
      gst_object_unref(pipeline);
  }

  // An error the pipeline posted (no decoder for the caps, a decode error).
  bool failed(std::string* error) {
    GstMessage* m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (m == nullptr)
      return false;
    GError* e = nullptr;
    gst_message_parse_error(m, &e, nullptr);
    const std::string why =
        std::string("GStreamer: ") + (e != nullptr ? e->message : "?");
    // Callers poll and may drop the text; say it once here.
    log::w(kTag, "{}", why);
    if (error != nullptr)
      *error = why;
    g_clear_error(&e);
    gst_message_unref(m);
    return true;
  }
};

GstCodec::GstCodec() : d_(std::make_unique<Impl>()) {}
GstCodec::~GstCodec() = default;

std::unique_ptr<GstCodec> GstCodec::configure(const std::string& mime,
                                              const std::uint8_t* csd,
                                              std::size_t csd_size,
                                              std::string* error,
                                              const std::uint8_t* first,
                                              std::size_t first_size) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<GstCodec> {
    if (error)
      *error = m;
    return nullptr;
  };
  const char* in_caps = caps_for_mime(mime);
  if (in_caps == nullptr)
    return fail("unsupported MIME");
  if (!gst_ready())
    return fail("GStreamer unavailable");

  auto dec = std::unique_ptr<GstCodec>(new GstCodec());
  Impl* d = dec->d_.get();
  // appsrc ! [parser] ! decoder ! appsink, linked before anything flows. An
  // autoplugging bin would pick the same elements, but it links the decoder's
  // output only after the decoder has negotiated, and a vendor decoder that
  // has by then chosen its own compressed layout does not always change its
  // mind. Linked up front, the decoder sees what the appsink takes.
  GstCaps* caps = gst_caps_from_string(in_caps);
  std::string dec_name;
  GstElement* parser = pick(
      GST_ELEMENT_FACTORY_TYPE_PARSER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
      caps, nullptr);
  // The stream's format, from the csd or else its first access unit. A
  // decoder is picked by it: one that gives nothing deeper than 8 bits is
  // passed over for a deeper stream, and for an H.264 profile past 8-bit
  // 4:2:0 the caps name the profile, which a decoder that lists the
  // profiles it takes (VA's) is matched against.
  bool deep = false;
  GstCaps* dec_caps = gst_caps_copy(caps);
  if (mime == "video/hevc") {
    deep = hevc_deep(csd, csd_size) || hevc_deep(first, first_size);
  } else if (mime == "video/avc") {
    H264Format f;
    if (h264_format(csd, csd_size, &f) || h264_format(first, first_size, &f)) {
      deep = f.deep();
      if (const char* profile = h264_profile_past_8bit_420(f))
        gst_caps_set_simple(dec_caps, "profile", G_TYPE_STRING, profile,
                            nullptr);
    }
  }
  GstElement* decoder = pick(
      GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
      dec_caps, &dec_name, deep);
  gst_caps_unref(dec_caps);
  d->pipeline = gst_pipeline_new(nullptr);
  d->src = gst_element_factory_make("appsrc", nullptr);
  d->sink = gst_element_factory_make("appsink", nullptr);
  // Keep a reference each to the two ends; the bin owns the rest.
  if (d->src != nullptr)
    gst_object_ref_sink(d->src);
  if (d->sink != nullptr)
    gst_object_ref_sink(d->sink);
  if (decoder == nullptr || d->pipeline == nullptr || d->src == nullptr ||
      d->sink == nullptr) {
    for (GstElement* x : {parser, decoder})
      if (x != nullptr)
        gst_object_unref(x);
    gst_caps_unref(caps);
    return fail(decoder == nullptr ? "GStreamer has no decoder for " + mime
                                   : "GStreamer: no appsrc/appsink");
  }
  limit_threads(decoder);
  gst_bin_add_many(GST_BIN(d->pipeline), d->src, decoder, d->sink, nullptr);
  if (parser != nullptr)
    gst_bin_add(GST_BIN(d->pipeline), parser);
  d->bus = gst_element_get_bus(d->pipeline);

  g_object_set(d->src, "caps", caps, "format", GST_FORMAT_TIME, "block", FALSE,
               "max-bytes", static_cast<guint64>(0), nullptr);
  gst_caps_unref(caps);
  // NV12 or I420 in whatever memory the decoder has: a dma-buf is passed on
  // as it is and anything else is copied. Ahead of those, where GStreamer can
  // say it (1.24), the decoder's own surface with its DRM format and
  // modifier: a decoder whose surfaces are tiled (VA on most GPUs) has no
  // linear dma-buf to give and would otherwise copy every frame out itself.
  // The compositor is asked about the modifier where the frame is shown.
  const char* out_caps =
      "video/x-raw,format=(string){NV12,I420,P010_10LE,I420_10LE}";
#if GST_CHECK_VERSION(1, 24, 0)
  if (!copy_always())
    out_caps =
        "video/x-raw(memory:DMABuf),format=(string)DMA_DRM; "
        "video/x-raw,format=(string){NV12,I420,P010_10LE,I420_10LE}";
#endif
  if (GstPad* pad = gst_element_get_static_pad(d->sink, "sink")) {
    gst_pad_add_probe(
        pad,
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM |
                                     GST_PAD_PROBE_TYPE_PUSH),
        answer_allocation, nullptr, nullptr);
    gst_object_unref(pad);
  }
  caps = gst_caps_from_string(out_caps);
  g_object_set(d->sink, "caps", caps, "sync", FALSE, "max-buffers",
               kSinkBuffers, "drop", FALSE, "emit-signals", FALSE, nullptr);
  gst_caps_unref(caps);
  const bool linked =
      parser != nullptr
          ? gst_element_link_many(d->src, parser, decoder, d->sink, nullptr)
          : gst_element_link_many(d->src, decoder, d->sink, nullptr);
  if (!linked)
    return fail("GStreamer: " + dec_name + " does not link");

  if (csd != nullptr && csd_size > 0)
    d->csd.assign(reinterpret_cast<const char*>(csd), csd_size);
  d->avc = mime == "video/avc";
  d->hevc = mime == "video/hevc";
  d->read_color(csd, csd_size);
  if (gst_element_set_state(d->pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE)
    return fail("GStreamer pipeline did not start");
  d->last_progress_us = g_get_monotonic_time();
  log::i(kTag, "decoder configured {} with {}", mime, dec_name);
  return dec;
}

int GstCodec::send_packet(const std::uint8_t* data,
                          std::size_t size,
                          std::int64_t pts_us,
                          std::string* error) {
  Impl* d = d_.get();
  if (d->failed(error))
    return -1;
  if (data == nullptr || size == 0) {  // end of stream: drain
    if (!d->eos_sent) {
      d->eos_sent = true;
      if (gst_app_src_end_of_stream(GST_APP_SRC(d->src)) != GST_FLOW_OK) {
        if (error)
          *error = "GStreamer: end-of-stream refused";
        return -1;
      }
    }
    return 0;
  }
  if (gst_app_src_get_current_level_bytes(GST_APP_SRC(d->src)) >
      kMaxQueuedBytes)
    return 1;  // receive first, then send this one again
  if (d->sent - d->got >= d->in_flight_max) {
    const gint64 now = g_get_monotonic_time();
    if (d->in_flight_max >= kInFlightMax ||
        now - d->last_progress_us < kStarvedUs)
      return 1;
    ++d->in_flight_max;  // nothing came out for a while: it wants more input
    d->last_progress_us = now;
  }

  const std::size_t head = d->csd_sent ? 0 : d->csd.size();
  GstBuffer* buf = gst_buffer_new_allocate(nullptr, head + size, nullptr);
  if (buf == nullptr) {
    if (error)
      *error = "out of memory";
    return -1;
  }
  if (head > 0)
    gst_buffer_fill(buf, 0, d->csd.data(), head);
  gst_buffer_fill(buf, head, data, size);
  d->csd_sent = true;
  d->read_color(data, size);
  if (pts_us >= 0)
    GST_BUFFER_PTS(buf) = static_cast<GstClockTime>(pts_us) * GST_USECOND;
  // Takes the buffer.
  const GstFlowReturn r = gst_app_src_push_buffer(GST_APP_SRC(d->src), buf);
  if (r != GST_FLOW_OK) {
    if (!d->failed(error) && error)
      *error = std::string("GStreamer: push ") + gst_flow_get_name(r);
    return -1;
  }
  ++d->sent;
  return 0;
}

int GstCodec::receive_frame(gfx::DmabufFrame* out,
                            std::int64_t* pts_us,
                            std::string* error) {
  auto err = [&](const std::string& m) -> int {
    if (error)
      *error = m;
    return -1;
  };
  if (out == nullptr)
    return err("null out");
  Impl* d = d_.get();
  if (d->failed(error))
    return -1;

  // The pipeline decodes on its own threads, so a frame may be a moment
  // behind the packet that completes it. Wait briefly when one is owed, and
  // longer once the stream has ended, where "none" has to mean drained.
  GstSample* s = nullptr;
  if (d->eos_sent) {
    for (int i = 0; i < 40 && s == nullptr; ++i) {
      s = gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink), 50 * GST_MSECOND);
      if (s == nullptr && gst_app_sink_is_eos(GST_APP_SINK(d->sink)))
        break;
    }
  } else {
    s = gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink),
                                     d->sent > d->got ? 4 * GST_MSECOND : 0);
  }
  if (s == nullptr)
    return d->failed(error) ? -1 : 0;

  // Dropped on every path out but the zero-copy one, which keeps it for as
  // long as the frame is in use.
  std::shared_ptr<void> sample(
      s, [](void* p) { gst_sample_unref(static_cast<GstSample*>(p)); });
  GstBuffer* buf = gst_sample_get_buffer(s);
  GstCaps* caps = gst_sample_get_caps(s);
  if (buf == nullptr || caps == nullptr)
    return err("GStreamer: sample without video caps");
#if GST_CHECK_VERSION(1, 24, 0)
  if (gst_video_is_dma_drm_caps(caps)) {
    // There is no copy to fall back on: such a buffer does not map as a
    // picture.
    gfx::DmabufFrame f{};
    std::string why;
    if (drm_frame(buf, caps, d->said(), &f, &why) != 1)
      return err(why);
    ++d->got;
    d->last_progress_us = g_get_monotonic_time();
    if (pts_us != nullptr)
      *pts_us =
          GST_BUFFER_PTS_IS_VALID(buf)
              ? static_cast<std::int64_t>(GST_BUFFER_PTS(buf) / GST_USECOND)
              : 0;
    if (d->got == 1)
      log::i(kTag,
             "frames {}x{}: the decoder's own dma-bufs, format 0x{:x} "
             "modifier 0x{:x}, no copy",
             f.width, f.height, f.drm_format, f.modifier);
    f.hold = std::move(sample);
    *out = f;
    return 1;
  }
#endif
  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps))
    return err("GStreamer: sample without video caps");
  const GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&info);
  // 10 bits: P010 as it is, or planar with the bits at the low end.
  const bool deep =
      fmt == GST_VIDEO_FORMAT_P010_10LE || fmt == GST_VIDEO_FORMAT_I420_10LE;
  if (fmt != GST_VIDEO_FORMAT_NV12 && fmt != GST_VIDEO_FORMAT_I420 && !deep)
    return err("GStreamer: frame is not 8-bit or 10-bit 4:2:0");
  const int w = GST_VIDEO_INFO_WIDTH(&info);
  const int h = GST_VIDEO_INFO_HEIGHT(&info);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
    return err("GStreamer: bad frame size");
  ++d->got;
  d->last_progress_us = g_get_monotonic_time();
  if (pts_us != nullptr)
    *pts_us = GST_BUFFER_PTS_IS_VALID(buf)
                  ? static_cast<std::int64_t>(GST_BUFFER_PTS(buf) / GST_USECOND)
                  : 0;

  // Zero-copy: one NV12 dma-buf. The plane layout is the buffer's video
  // meta where it has one (a hardware decoder pads rows and lines), else what
  // the caps imply. The dma-buf is the buffer's memory, or the one a vendor
  // decoder names beside a mapping of it (tagged_fd).
  GstMemory* mem = gst_buffer_peek_memory(buf, 0);
  const GstVideoMeta* vm = gst_buffer_get_video_meta(buf);
  int fd = -1;
  gsize mem_off = 0;
  bool tagged = false;
  if (gst_buffer_n_memory(buf) == 1 && gst_is_dmabuf_memory(mem)) {
    gsize mem_max = 0;
    gst_memory_get_sizes(mem, &mem_off, &mem_max);
    fd = gst_dmabuf_memory_get_fd(mem);
  } else if (tagged_fd(vm) >= 0) {
    if (d->tagged_out->load(std::memory_order_relaxed) <
        d->tagged_limit(tagged_fd(vm), w, h)) {
      fd = tagged_fd(vm);
      tagged = true;
    } else {
      ++d->tagged_copied;
      d->tally();
    }
  }
  if (!copy_always() && fmt == GST_VIDEO_FORMAT_NV12 && fd >= 0) {
    gfx::DmabufFrame f{};
    f.fd = fd;
    f.width = static_cast<std::uint32_t>(w);
    f.height = static_cast<std::uint32_t>(h);
    f.drm_format = DRM_FORMAT_NV12;
    f.modifier = DRM_FORMAT_MOD_LINEAR;
    f.plane_count = 2;
    bool sane = f.fd >= 0;
    for (int i = 0; i < 2; ++i) {
      const gsize off =
          (vm != nullptr ? vm->offset[i]
                         : GST_VIDEO_INFO_PLANE_OFFSET(&info, i)) +
          mem_off;
      const int stride =
          vm != nullptr ? vm->stride[i] : GST_VIDEO_INFO_PLANE_STRIDE(&info, i);
      if (stride < w || off > 0xffffffffu)
        sane = false;
      f.planes[i].offset = static_cast<std::uint32_t>(off);
      f.planes[i].stride = static_cast<std::uint32_t>(stride > 0 ? stride : 0);
    }
    if (sane) {
      const off_t size = ::lseek(f.fd, 0, SEEK_END);
      const std::uint64_t need =
          f.planes[1].offset +
          static_cast<std::uint64_t>(f.planes[1].stride) * ((h + 1) / 2);
      f.mem_size = size > 0 ? static_cast<std::uint64_t>(size) : need;
      // A descriptor taken from a tag has to show it is a buffer of that
      // size; one that will not say its size is not passed on.
      sane = need <= f.mem_size && (!tagged || size > 0);
    }
    if (sane) {
      if (d->got == 1)
        log::i(kTag, "frames {}x{}: the decoder's own NV12 dma-bufs{}, no copy",
               w, h, tagged ? " (named in the video meta)" : "");
      if (tagged) {
        ++d->tagged_passed;
        d->tally();
        d->tagged_out->fetch_add(1, std::memory_order_relaxed);
        f.hold = std::shared_ptr<void>(
            sample.get(), [keep = sample, n = d->tagged_out](void*) mutable {
              keep.reset();
              n->fetch_sub(1, std::memory_order_relaxed);
            });
      } else {
        f.hold = std::move(sample);
      }
      note_color(info, d->said(), &f);
      *out = f;
      return 1;
    }
  }

  // Otherwise the frame is in memory the compositor cannot take: copy it.
  GstVideoFrame vf;
  if (!gst_video_frame_map(&vf, &info, buf, GST_MAP_READ))
    return err("GStreamer: frame cannot be mapped");
  auto pool = d->sw_pool;
  const auto uw = static_cast<std::uint32_t>(w);
  const auto uh = static_cast<std::uint32_t>(h);
  if (pool == nullptr || pool->width() != uw || pool->height() != uh ||
      pool->deep() != deep) {
    pool = gfx::SwFramePool::create(uw, uh, error, deep);
    if (pool != nullptr)
      log::i(kTag, "frames {}x{}: copied to {} dma-bufs (from {} memory)", w, h,
             deep ? "P010" : "NV12",
             mem != nullptr && mem->allocator != nullptr &&
                     mem->allocator->mem_type != nullptr
                 ? mem->allocator->mem_type
                 : "unnamed");
    d->sw_pool = pool;
  }
  bool ok = pool != nullptr;
  if (ok) {
    const bool nv12 =
        fmt == GST_VIDEO_FORMAT_NV12 || fmt == GST_VIDEO_FORMAT_P010_10LE;
    const std::uint8_t* const planes[3] = {
        static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 0)),
        static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 1)),
        nv12 ? nullptr
             : static_cast<const std::uint8_t*>(
                   GST_VIDEO_FRAME_PLANE_DATA(&vf, 2))};
    const int strides[3] = {GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0),
                            GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 1),
                            nv12 ? 0 : GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 2)};
    // Planar 10-bit has its bits at the low end of each 16; P010 at the top.
    ok = deep ? pool->write_420_16(planes, strides, nv12, nv12 ? 0 : 6, out,
                                   error)
              : pool->write_420(planes, strides, nv12, out, error);
  }
  gst_video_frame_unmap(&vf);
  if (ok)
    note_color(info, d->said(), out);
  return ok ? 1 : -1;
}

void GstCodec::flush() {
  Impl* d = d_.get();
  d->pumping([d] {
    gst_element_send_event(d->pipeline, gst_event_new_flush_start());
    gst_element_send_event(d->pipeline, gst_event_new_flush_stop(TRUE));
  });
  while (GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink), 0))
    gst_sample_unref(s);
  d->eos_sent = false;
  d->sent = 0;
  d->got = 0;
  // A decoder may come back from a flush with other buffers; count again.
  d->tagged_fds.clear();
  d->last_progress_us = g_get_monotonic_time();
}

}  // namespace acl::media

#endif  // ACL_HAVE_GL && ACL_HAVE_GST

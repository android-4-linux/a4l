// SPDX-License-Identifier: Apache-2.0
// Provenance: GStreamer public API documentation (appsrc, appsink,
// GstElementFactory, GstVideoMeta, gstdmabuf.h). GStreamer is
// LGPL-2.1-or-later, dynamically linked. No GPL-3.0 sources.

#include "media/gst_codec.h"

#if ACL_HAVE_GL && ACL_HAVE_GST

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "gfx/sw_frame_pool.h"
#include "log/log.h"

namespace acl::media {
namespace {
constexpr const char* kTag = "acl.media.gst";

// Input the pipeline may hold before send_packet says "receive first". The
// appsink holds kSinkBuffers decoded frames and then blocks the decoder, so
// without this the encoded stream would pile up in appsrc without bound.
constexpr guint64 kMaxQueuedBytes = 2u * 1024u * 1024u;
constexpr guint kSinkBuffers = 4;
// Access units the pipeline may hold that have not come out as frames. A
// MediaCodec takes input only about as fast as it gives output, and a player
// counts on that: what is inside the codec is what a seek throws away. The
// allowance starts small and grows only when the decoder shows it needs more
// before it will emit anything (frame reordering).
constexpr std::uint64_t kInFlightStart = 4;
constexpr std::uint64_t kInFlightMax = 32;
constexpr gint64 kStarvedUs = 50 * 1000;

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

// The highest-ranked installed element of `type` that takes `caps`, as an
// autoplugger would choose. nullptr if there is none. *name gets its factory
// name, with "(hardware)" when it says it is one.
GstElement* pick(GstElementFactoryListType type,
                 const GstCaps* caps,
                 std::string* name) {
  GList* all = gst_element_factory_list_get_elements(type, GST_RANK_MARGINAL);
  GList* fit = gst_element_factory_list_filter(all, caps, GST_PAD_SINK, FALSE);
  GstElement* e = nullptr;
  for (GList* l = fit; l != nullptr && e == nullptr; l = l->next) {
    auto* f = GST_ELEMENT_FACTORY(l->data);
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

struct GstCodec::Impl {
  GstElement* pipeline = nullptr;
  GstElement* src = nullptr;   // appsrc
  GstElement* sink = nullptr;  // appsink
  GstBus* bus = nullptr;
  std::string csd;  // sent ahead of the first access unit
  bool csd_sent = false;
  bool eos_sent = false;
  std::uint64_t sent = 0;  // access units in
  std::uint64_t got = 0;   // frames out
  std::uint64_t in_flight_max = kInFlightStart;
  gint64 last_progress_us = 0;  // monotonic: last frame out, or allowance raise
  std::shared_ptr<gfx::SwFramePool> sw_pool;

  ~Impl() {
    if (pipeline != nullptr)
      gst_element_set_state(pipeline, GST_STATE_NULL);
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
                                              std::string* error) {
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
  GstElement* decoder = pick(
      GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
      caps, &dec_name);
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
  // as it is and anything else is copied. A vendor's compressed layout is not
  // asked for: linux-dmabuf could not describe it.
  caps = gst_caps_from_string("video/x-raw,format=(string){NV12,I420}");
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
  GstVideoInfo info;
  if (buf == nullptr || caps == nullptr ||
      !gst_video_info_from_caps(&info, caps))
    return err("GStreamer: sample without video caps");
  const GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&info);
  if (fmt != GST_VIDEO_FORMAT_NV12 && fmt != GST_VIDEO_FORMAT_I420)
    return err("GStreamer: frame is not NV12 or I420");
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
  // the caps imply.
  static const bool copy_always = std::getenv("ACL_VIDEO_GST_COPY") != nullptr;
  GstMemory* mem = gst_buffer_peek_memory(buf, 0);
  if (!copy_always && fmt == GST_VIDEO_FORMAT_NV12 &&
      gst_buffer_n_memory(buf) == 1 && gst_is_dmabuf_memory(mem)) {
    const GstVideoMeta* vm = gst_buffer_get_video_meta(buf);
    gsize mem_off = 0;
    gsize mem_max = 0;
    gst_memory_get_sizes(mem, &mem_off, &mem_max);
    gfx::DmabufFrame f{};
    f.fd = gst_dmabuf_memory_get_fd(mem);
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
      sane = need <= f.mem_size;
    }
    if (sane) {
      if (d->got == 1)
        log::i(kTag, "frames {}x{}: the decoder's own NV12 dma-bufs, no copy",
               w, h);
      f.hold = std::move(sample);
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
  if (pool == nullptr || pool->width() != uw || pool->height() != uh) {
    pool = gfx::SwFramePool::create(uw, uh, error);
    if (pool != nullptr)
      log::i(kTag, "frames {}x{}: copied to NV12 dma-bufs", w, h);
    d->sw_pool = pool;
  }
  bool ok = pool != nullptr;
  if (ok) {
    const bool nv12 = fmt == GST_VIDEO_FORMAT_NV12;
    const std::uint8_t* const planes[3] = {
        static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 0)),
        static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 1)),
        nv12 ? nullptr
             : static_cast<const std::uint8_t*>(
                   GST_VIDEO_FRAME_PLANE_DATA(&vf, 2))};
    const int strides[3] = {GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0),
                            GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 1),
                            nv12 ? 0 : GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 2)};
    ok = pool->write_420(planes, strides, nv12, out, error);
  }
  gst_video_frame_unmap(&vf);
  return ok ? 1 : -1;
}

void GstCodec::flush() {
  Impl* d = d_.get();
  gst_element_send_event(d->pipeline, gst_event_new_flush_start());
  gst_element_send_event(d->pipeline, gst_event_new_flush_stop(TRUE));
  while (GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink), 0))
    gst_sample_unref(s);
  d->eos_sent = false;
  d->sent = 0;
  d->got = 0;
  d->last_progress_us = g_get_monotonic_time();
}

}  // namespace acl::media

#endif  // ACL_HAVE_GL && ACL_HAVE_GST

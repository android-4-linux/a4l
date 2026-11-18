// SPDX-License-Identifier: Apache-2.0
// Provenance: FFmpeg public API (libavcodec/libavutil, LGPL, dynamic link only:
// dlopen'd RTLD_LOCAL, as media/video_decoder.cpp does); the Android
// MediaFormat conventions for audio codec-specific data as documented for
// android.media.MediaCodec; RFC 7845 and the Vorbis I specification for the
// header layouts. No GPL-3.0 sources.

#include "media/audio_decoder.h"

#include "acl_config.h"

#if ACL_HAVE_FFMPEG

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

// AVChannelLayout replaced the channel count fields in libavutil 57.28
// (FFmpeg 5.1). Older libraries, as on a board with FFmpeg 4, have only the
// count.
#define ACL_FF_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100))

#include "log/log.h"

namespace acl::media {
namespace {

constexpr const char* kTag = "acl.media.audio";

// FFmpeg resolved at run time and kept private to this file (RTLD_LOCAL), so
// its transitive dependencies do not meet an app's own copies.
struct Ff {
#define FF_FN(n) decltype(&::n) n = nullptr
  FF_FN(av_mallocz);
  FF_FN(av_strerror);
  FF_FN(av_frame_alloc);
  FF_FN(av_frame_free);
  FF_FN(av_frame_unref);
  FF_FN(av_packet_alloc);
  FF_FN(av_packet_free);
  FF_FN(av_packet_unref);
#if ACL_FF_CH_LAYOUT
  FF_FN(av_channel_layout_default);
#endif
  FF_FN(avcodec_find_decoder);
  FF_FN(avcodec_alloc_context3);
  FF_FN(avcodec_open2);
  FF_FN(avcodec_free_context);
  FF_FN(avcodec_send_packet);
  FF_FN(avcodec_receive_frame);
  FF_FN(avcodec_flush_buffers);
#undef FF_FN
  bool ok = false;
};

void* ff_open(const char* base, int major) {
  char name[64];
  std::snprintf(name, sizeof(name), "%s.%d", base, major);
  if (void* h = ::dlopen(name, RTLD_NOW | RTLD_LOCAL))
    return h;
  return ::dlopen(base, RTLD_NOW | RTLD_LOCAL);
}

const Ff& ffmpeg() {
  static Ff ff;
  static std::once_flag once;
  std::call_once(once, [] {
    void* libs[2] = {ff_open("libavutil.so", LIBAVUTIL_VERSION_MAJOR),
                     ff_open("libavcodec.so", LIBAVCODEC_VERSION_MAJOR)};
    if (libs[0] == nullptr || libs[1] == nullptr) {
      log::w(kTag, "FFmpeg could not be loaded; audio decode is off");
      return;
    }
    bool all = true;
    const auto sym = [&](const char* n) -> void* {
      for (void* lib : libs) {
        if (void* s = ::dlsym(lib, n))
          return s;
      }
      all = false;
      log::w(kTag, "FFmpeg has no {}", n);
      return nullptr;
    };
#define FF_BIND(n) ff.n = reinterpret_cast<decltype(ff.n)>(sym(#n))
    FF_BIND(av_mallocz);
    FF_BIND(av_strerror);
    FF_BIND(av_frame_alloc);
    FF_BIND(av_frame_free);
    FF_BIND(av_frame_unref);
    FF_BIND(av_packet_alloc);
    FF_BIND(av_packet_free);
    FF_BIND(av_packet_unref);
#if ACL_FF_CH_LAYOUT
    FF_BIND(av_channel_layout_default);
#endif
    FF_BIND(avcodec_find_decoder);
    FF_BIND(avcodec_alloc_context3);
    FF_BIND(avcodec_open2);
    FF_BIND(avcodec_free_context);
    FF_BIND(avcodec_send_packet);
    FF_BIND(avcodec_receive_frame);
    FF_BIND(avcodec_flush_buffers);
#undef FF_BIND
    ff.ok = all;
  });
  return ff;
}

std::string ff_error(int code) {
  char buf[128] = {0};
  if (ffmpeg().av_strerror != nullptr)
    ffmpeg().av_strerror(code, buf, sizeof(buf));
  return buf;
}

AVCodecID codec_of(std::string_view mime) noexcept {
  if (mime == "audio/mp4a-latm")
    return AV_CODEC_ID_AAC;
  if (mime == "audio/mpeg")
    return AV_CODEC_ID_MP3;
  if (mime == "audio/opus")
    return AV_CODEC_ID_OPUS;
  if (mime == "audio/vorbis")
    return AV_CODEC_ID_VORBIS;
  if (mime == "audio/flac")
    return AV_CODEC_ID_FLAC;
  return AV_CODEC_ID_NONE;
}

// What the decoder is given as its out-of-band setup, built from the
// MediaFormat's csd buffers. Android and FFmpeg agree for most codecs and
// differ for Vorbis.
//   AAC     csd-0 is the AudioSpecificConfig. FFmpeg takes it as is.
//   Opus    csd-0 is the OpusHead identification header; csd-1 and csd-2 are
//           the pre-skip and seek pre-roll as nanoseconds, which the header
//           already carries. FFmpeg takes csd-0.
//   FLAC    csd-0 is "fLaC" followed by the STREAMINFO metadata block.
//           FFmpeg takes that form, and the bare 34-byte block as well.
//   Vorbis  csd-0 is the identification header and csd-1 the setup header.
//           FFmpeg wants the three header packets of the stream in one
//           buffer, with Xiph lacing: a count byte of 2, the lengths of the
//           first two, then the three packets. The comment header is not in
//           the MediaFormat; an empty one is supplied.
//   MP3     none.
bool build_extradata(AVCodecID id,
                     const std::vector<std::vector<std::uint8_t>>& csd,
                     std::vector<std::uint8_t>* out,
                     std::string* error) {
  out->clear();
  if (id == AV_CODEC_ID_MP3)
    return true;
  if (csd.empty() || csd[0].empty()) {
    if (id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_FLAC)
      return true;  // in-band configuration (ADTS, a FLAC frame header) may do
    *error = "the format has no csd-0";
    return false;
  }
  if (id != AV_CODEC_ID_VORBIS) {
    *out = csd[0];
    return true;
  }
  if (csd.size() < 2 || csd[1].empty()) {
    *error = "a Vorbis format needs csd-0 and csd-1";
    return false;
  }
  const std::vector<std::uint8_t>& ident = csd[0];
  const std::vector<std::uint8_t>& setup = csd[1];
  // A comment header with an empty vendor string and no comments.
  static constexpr std::uint8_t kComment[] = {
      0x03, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
  const auto lace = [&](std::size_t n) {
    for (; n >= 255; n -= 255)
      out->push_back(255);
    out->push_back(static_cast<std::uint8_t>(n));
  };
  out->push_back(2);
  lace(ident.size());
  lace(sizeof(kComment));
  out->insert(out->end(), ident.begin(), ident.end());
  out->insert(out->end(), kComment, kComment + sizeof(kComment));
  out->insert(out->end(), setup.begin(), setup.end());
  return true;
}

std::int16_t s16_of(float v) noexcept {
  if (!(v == v))
    return 0;
  return static_cast<std::int16_t>(
      std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

// One sample of `fmt` at index `i` of `plane`, as 16-bit.
std::int16_t sample_at(AVSampleFormat fmt, const std::uint8_t* plane, int i) {
  switch (fmt) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
      return static_cast<std::int16_t>((static_cast<int>(plane[i]) - 128) *
                                       256);
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: {
      std::int16_t v;
      std::memcpy(&v, plane + i * 2, 2);
      return v;
    }
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: {
      std::int32_t v;
      std::memcpy(&v, plane + i * 4, 4);
      return static_cast<std::int16_t>(v >> 16);
    }
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: {
      float v;
      std::memcpy(&v, plane + i * 4, 4);
      return s16_of(v);
    }
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP: {
      double v;
      std::memcpy(&v, plane + i * 8, 8);
      return s16_of(static_cast<float>(v));
    }
    default:
      return 0;
  }
}

bool planar(AVSampleFormat fmt) noexcept {
  return fmt == AV_SAMPLE_FMT_U8P || fmt == AV_SAMPLE_FMT_S16P ||
         fmt == AV_SAMPLE_FMT_S32P || fmt == AV_SAMPLE_FMT_FLTP ||
         fmt == AV_SAMPLE_FMT_DBLP;
}

bool convertible(AVSampleFormat fmt) noexcept {
  switch (fmt) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
      return true;
    default:
      return false;
  }
}

}  // namespace

struct AudioDecoder::Impl {
  AVCodecContext* ctx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  bool draining = false;
  ~Impl() {
    const Ff& ff = ffmpeg();
    if (!ff.ok)
      return;
    if (packet != nullptr)
      ff.av_packet_free(&packet);
    if (frame != nullptr)
      ff.av_frame_free(&frame);
    if (ctx != nullptr)
      ff.avcodec_free_context(&ctx);
  }
};

AudioDecoder::AudioDecoder() : d_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::known(std::string_view mime) noexcept {
  return codec_of(mime) != AV_CODEC_ID_NONE;
}

bool AudioDecoder::available(std::string_view mime) {
  const AVCodecID id = codec_of(mime);
  const Ff& ff = ffmpeg();
  return id != AV_CODEC_ID_NONE && ff.ok &&
         ff.avcodec_find_decoder(id) != nullptr;
}

std::unique_ptr<AudioDecoder> AudioDecoder::configure(const Config& config,
                                                      std::string* error) {
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  const AVCodecID id = codec_of(config.mime);
  if (id == AV_CODEC_ID_NONE) {
    *error = "no audio decoder for " + config.mime;
    return nullptr;
  }
  const Ff& ff = ffmpeg();
  if (!ff.ok) {
    *error = "FFmpeg is not available";
    return nullptr;
  }
  const AVCodec* codec = ff.avcodec_find_decoder(id);
  if (codec == nullptr) {
    *error = "this FFmpeg has no decoder for " + config.mime;
    return nullptr;
  }
  if (config.sample_rate < 0 || config.sample_rate > 768000 ||
      config.channels < 0 || config.channels > kMaxAudioDecodeChannels) {
    *error = "sample rate or channel count out of range";
    return nullptr;
  }
  std::vector<std::uint8_t> extra;
  if (!build_extradata(id, config.csd, &extra, error))
    return nullptr;
  // A csd of megabytes is not a header.
  if (extra.size() > (1u << 20)) {
    *error = "codec-specific data too large";
    return nullptr;
  }

  std::unique_ptr<AudioDecoder> dec(new AudioDecoder());
  Impl& d = *dec->d_;
  d.ctx = ff.avcodec_alloc_context3(codec);
  d.frame = ff.av_frame_alloc();
  d.packet = ff.av_packet_alloc();
  if (d.ctx == nullptr || d.frame == nullptr || d.packet == nullptr) {
    *error = "out of memory";
    return nullptr;
  }
  if (config.sample_rate > 0)
    d.ctx->sample_rate = config.sample_rate;
#if ACL_FF_CH_LAYOUT
  if (config.channels > 0)
    ff.av_channel_layout_default(&d.ctx->ch_layout, config.channels);
#else
  if (config.channels > 0)
    d.ctx->channels = config.channels;
#endif
  if (!extra.empty()) {
    // FFmpeg reads past the end by design and wants zeroed padding there.
    auto* mem = static_cast<std::uint8_t*>(
        ff.av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (mem == nullptr) {
      *error = "out of memory";
      return nullptr;
    }
    std::memcpy(mem, extra.data(), extra.size());
    d.ctx->extradata = mem;  // freed with the context
    d.ctx->extradata_size = static_cast<int>(extra.size());
  }
  // Timestamps are microseconds, as MediaCodec's are. Told so, a decoder
  // that trims its own start (Opus pre-skip) keeps them right.
  d.ctx->pkt_timebase = AVRational{1, 1000000};
  // One thread: frames are small, and a decoder per track is already a
  // thread's worth.
  d.ctx->thread_count = 1;
  const int rc = ff.avcodec_open2(d.ctx, codec, nullptr);
  if (rc < 0) {
    *error = "avcodec_open2: " + ff_error(rc);
    return nullptr;
  }
  log::i(kTag, "audio decode ready: {} via FFmpeg {} ({} Hz, {} ch hinted)",
         config.mime, codec->name, config.sample_rate, config.channels);
  return dec;
}

int AudioDecoder::send_packet(const std::uint8_t* data,
                              std::size_t size,
                              std::int64_t pts_us,
                              std::string* error) {
  const Ff& ff = ffmpeg();
  Impl& d = *d_;
  if (data == nullptr || size == 0) {
    if (d.draining)
      return 0;
    const int rc = ff.avcodec_send_packet(d.ctx, nullptr);
    if (rc == AVERROR(EAGAIN))
      return 1;
    d.draining = true;
    return 0;
  }
  if (size > (64u << 20)) {
    if (error != nullptr)
      *error = "access unit too large";
    return -1;
  }
  // The packet's own buffer, padded: the caller's is not, and FFmpeg's
  // bitstream readers run a few bytes past the end.
  std::vector<std::uint8_t> padded(size + AV_INPUT_BUFFER_PADDING_SIZE);
  std::memcpy(padded.data(), data, size);
  ff.av_packet_unref(d.packet);
  d.packet->data = padded.data();
  d.packet->size = static_cast<int>(size);
  d.packet->pts = pts_us;
  d.packet->dts = pts_us;
  const int rc = ff.avcodec_send_packet(d.ctx, d.packet);
  d.packet->data = nullptr;
  d.packet->size = 0;
  if (rc == AVERROR(EAGAIN))
    return 1;
  if (rc == AVERROR_EOF) {
    if (error != nullptr)
      *error = "input after end of stream";
    return -1;
  }
  if (rc < 0) {
    // Damaged or unparseable: FFmpeg has dropped it. Go on with the next.
    static int logged = 0;
    if (logged++ < 4)
      log::w(kTag, "an access unit was not decodable ({}); dropped",
             ff_error(rc));
  }
  return 0;
}

AudioDecoder::Result AudioDecoder::receive(Frame* out, std::string* error) {
  const Ff& ff = ffmpeg();
  Impl& d = *d_;
  for (;;) {
    const int rc = ff.avcodec_receive_frame(d.ctx, d.frame);
    if (rc == AVERROR(EAGAIN))
      return Result::Again;
    if (rc == AVERROR_EOF)
      return Result::EndOfStream;
    if (rc < 0) {
      if (error != nullptr)
        *error = "avcodec_receive_frame: " + ff_error(rc);
      return Result::Error;
    }
    const AVFrame& f = *d.frame;
    const auto fmt = static_cast<AVSampleFormat>(f.format);
#if ACL_FF_CH_LAYOUT
    const int channels = f.ch_layout.nb_channels;
#else
    const int channels = f.channels;
#endif
    const int samples = f.nb_samples;
    // What the stream says about itself is checked before it sizes anything.
    if (!convertible(fmt) || channels < 1 ||
        channels > kMaxAudioDecodeChannels || samples < 1 ||
        samples > kMaxAudioDecodeFrames || f.sample_rate < 1) {
      ff.av_frame_unref(d.frame);
      continue;  // not audio this can pass on; skip it
    }
    out->pcm.resize(static_cast<std::size_t>(samples) *
                    static_cast<std::size_t>(channels));
    if (planar(fmt)) {
      // Planes past the eighth live in extended_data; with at most eight
      // channels they are all in extended_data either way.
      for (int c = 0; c < channels; ++c) {
        const std::uint8_t* plane = f.extended_data[c];
        if (plane == nullptr)
          continue;
        for (int i = 0; i < samples; ++i)
          out->pcm[static_cast<std::size_t>(i) * channels + c] =
              sample_at(fmt, plane, i);
      }
    } else {
      const std::uint8_t* plane = f.extended_data[0];
      for (int i = 0; i < samples * channels; ++i)
        out->pcm[static_cast<std::size_t>(i)] = sample_at(fmt, plane, i);
    }
    out->sample_rate = f.sample_rate;
    out->channels = channels;
    out->pts_us = f.pts == AV_NOPTS_VALUE ? 0 : f.pts;
    ff.av_frame_unref(d.frame);
    return Result::Frame;
  }
}

void AudioDecoder::flush() {
  const Ff& ff = ffmpeg();
  if (d_->ctx != nullptr)
    ff.avcodec_flush_buffers(d_->ctx);
  d_->draining = false;
}

}  // namespace acl::media

#else  // !ACL_HAVE_FFMPEG

namespace acl::media {

struct AudioDecoder::Impl {};
AudioDecoder::AudioDecoder() = default;
AudioDecoder::~AudioDecoder() = default;
bool AudioDecoder::known(std::string_view mime) noexcept {
  return mime == "audio/mp4a-latm" || mime == "audio/mpeg" ||
         mime == "audio/opus" || mime == "audio/vorbis" || mime == "audio/flac";
}
bool AudioDecoder::available(std::string_view) {
  return false;
}
std::unique_ptr<AudioDecoder> AudioDecoder::configure(const Config&,
                                                      std::string* error) {
  if (error != nullptr)
    *error = "built without FFmpeg";
  return nullptr;
}
int AudioDecoder::send_packet(const std::uint8_t*,
                              std::size_t,
                              std::int64_t,
                              std::string*) {
  return -1;
}
AudioDecoder::Result AudioDecoder::receive(Frame*, std::string*) {
  return Result::Error;
}
void AudioDecoder::flush() {}

}  // namespace acl::media

#endif  // ACL_HAVE_FFMPEG

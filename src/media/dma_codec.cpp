// SPDX-License-Identifier: Apache-2.0
// Provenance: see dma_codec.h. No GPL-3.0 sources.

#include "media/dma_codec.h"

#if ACL_HAVE_DMABUF_CODEC

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "log/log.h"
#include "src/dma_decoder.h"
#include "src/dma_decoder_factory.h"

#ifndef V4L2_PIX_FMT_HEVC_SLICE
#define V4L2_PIX_FMT_HEVC_SLICE v4l2_fourcc('S', '2', '6', '5')
#endif

namespace acl::media {
namespace {

constexpr const char* kTag = "acl.media.dma";

// Access units kept until their frame comes out, to send again to the decoder
// that replaces one the stream outgrew. Units that never yield a frame fall
// off the front.
constexpr std::size_t kMaxInFlight = 32;
constexpr std::size_t kMaxInFlightBytes = std::size_t{16} << 20;
// A stream that keeps changing geometry before a frame comes out is not one
// a decoder is going to settle on.
constexpr int kMaxRebuildsWithoutFrame = 3;

std::uint32_t fourcc_for_mime(const std::string& mime) {
  if (mime == "video/avc")
    return V4L2_PIX_FMT_H264;
  if (mime == "video/hevc")
    return V4L2_PIX_FMT_HEVC;
  if (mime == "video/x-vnd.on2.vp8")
    return V4L2_PIX_FMT_VP8;
  if (mime == "video/x-vnd.on2.vp9")
    return V4L2_PIX_FMT_VP9;
  return 0;
}

int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ::ioctl(fd, req, arg);
  } while (r < 0 && errno == EINTR);
  return r;
}

// The lowest-numbered /dev/videoN that is a memory-to-memory device taking
// `fourcc` as its input: a decoder for it. Empty if there is none. Opening a
// node to ask what it is starts nothing on it.
std::string find_m2m_decoder(std::uint32_t fourcc) {
  std::vector<int> nodes;
  if (DIR* dir = ::opendir("/dev")) {
    while (const dirent* e = ::readdir(dir)) {
      if (std::strncmp(e->d_name, "video", 5) != 0)
        continue;
      char* end = nullptr;
      const long n = std::strtol(e->d_name + 5, &end, 10);
      if (end != e->d_name + 5 && *end == '\0' && n >= 0 && n < 4096)
        nodes.push_back(static_cast<int>(n));
    }
    ::closedir(dir);
  }
  std::sort(nodes.begin(), nodes.end());
  for (const int n : nodes) {
    const std::string path = "/dev/video" + std::to_string(n);
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;
    bool found = false;
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
      const std::uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                                     ? cap.device_caps
                                     : cap.capabilities;
      std::uint32_t type = 0;
      if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0)
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      else if ((caps & V4L2_CAP_VIDEO_M2M) != 0)
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      for (std::uint32_t i = 0; type != 0 && i < 64 && !found; ++i) {
        v4l2_fmtdesc desc{};
        desc.index = i;
        desc.type = type;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0)
          break;
        found = desc.pixelformat == fourcc;
      }
    }
    ::close(fd);
    if (found)
      return path;
  }
  return {};
}

}  // namespace

struct DmaCodec::Impl {
  // What a frame handed out needs to give its buffer back: the decoder, and
  // which decoder it was. A frame outlives a decoder that was replaced or
  // destroyed; its buffer then has nowhere to go back to.
  struct Shared {
    std::mutex mu;
    std::unique_ptr<v4l2wc::IDmaDecoder> dec;
    // Written under mu. Read without it by a frame being dropped, so that a
    // frame of a decoder already replaced does not wait behind the lock while
    // that decoder is torn down: the thread dropping it may be the one that
    // presents.
    std::atomic<std::uint64_t> generation{0};
    std::uint32_t frames_out = 0;
  };
  struct Token {
    std::shared_ptr<Shared> shared;
    std::uint64_t generation = 0;
    std::uint32_t index = 0;
    int fd = -1;
    ~Token() {
      if (shared->generation.load() == generation) {
        std::lock_guard<std::mutex> lock(shared->mu);
        if (shared->generation.load() == generation && shared->dec != nullptr) {
          shared->dec->Release(index);
          --shared->frames_out;
        }
      }
      if (fd >= 0)
        ::close(fd);
    }
  };
  struct Unit {
    std::int64_t pts = 0;
    std::vector<std::uint8_t> data;
  };

  std::shared_ptr<Shared> shared = std::make_shared<Shared>();
  Factory factory;
  std::vector<std::uint8_t> csd;
  bool csd_due = true;
  std::uint32_t width = 0, height = 0;  // as configured: the picture's size
  std::vector<std::uint8_t> scratch;    // csd + access unit
  std::deque<Unit> in_flight;           // sent, frame not yet out
  std::size_t in_flight_bytes = 0;
  std::deque<Unit> replay;      // to send to a replacement decoder
  bool change_pending = false;  // the stream outgrew the decoder
  int rebuilds_without_frame = 0;
  bool announced = false;

  // Everything below runs with shared->mu held.
  bool rebuild() {
    ++shared->generation;
    shared->frames_out = 0;
    shared->dec.reset();
    shared->dec = factory();
    return shared->dec != nullptr;
  }
  void remember(const std::uint8_t* data, std::size_t size, std::int64_t pts) {
    if (size > kMaxInFlightBytes)
      return;
    in_flight.push_back(Unit{pts, {data, data + size}});
    in_flight_bytes += size;
    while (in_flight.size() > kMaxInFlight ||
           in_flight_bytes > kMaxInFlightBytes) {
      in_flight_bytes -= in_flight.front().data.size();
      in_flight.pop_front();
    }
  }
  void forget(std::int64_t pts) {
    for (auto it = in_flight.begin(); it != in_flight.end(); ++it) {
      if (it->pts == pts) {
        in_flight_bytes -= it->data.size();
        in_flight.erase(it);
        return;
      }
    }
  }
  // Sends what the replaced decoder had not finished. False on an error.
  bool pump_replay() {
    while (!replay.empty()) {
      Unit& u = replay.front();
      const v4l2wc::SubmitResult r = shared->dec->SubmitBitstream(
          u.data.data(), u.data.size(), static_cast<std::uint64_t>(u.pts));
      if (r == v4l2wc::SubmitResult::kTryAgain)
        return true;
      if (r == v4l2wc::SubmitResult::kError)
        return false;
      if (r == v4l2wc::SubmitResult::kSourceChange) {
        change_pending = true;
        return true;
      }
      in_flight_bytes += u.data.size();
      in_flight.push_back(std::move(u));
      replay.pop_front();
    }
    return true;
  }
  // Replaces the decoder after a change of geometry and queues what it had
  // not finished. The stream carries its new parameter sets itself, so the
  // configured ones are not sent again.
  bool replace_for_change() {
    change_pending = false;
    if (++rebuilds_without_frame > kMaxRebuildsWithoutFrame)
      return false;
    log::i(kTag, "the stream changed geometry: new decoder, {} unit(s) resent",
           in_flight.size());
    replay = std::move(in_flight);
    in_flight.clear();
    in_flight_bytes = 0;
    csd_due = false;
    return rebuild();
  }
};

DmaCodec::DmaCodec() : d_(std::make_unique<Impl>()) {}

DmaCodec::~DmaCodec() {
  // The decoder goes now, on this thread. Frames still in use keep their
  // memory through their own descriptors and find no decoder to return to.
  std::unique_ptr<v4l2wc::IDmaDecoder> dec;
  {
    std::lock_guard<std::mutex> lock(d_->shared->mu);
    ++d_->shared->generation;
    dec = std::move(d_->shared->dec);
  }
}

std::unique_ptr<DmaCodec> DmaCodec::with_factory(Factory factory,
                                                 const std::uint8_t* csd,
                                                 std::size_t csd_size,
                                                 std::string* error) {
  auto codec = std::unique_ptr<DmaCodec>(new DmaCodec());
  Impl* d = codec->d_.get();
  d->factory = std::move(factory);
  if (csd != nullptr && csd_size > 0)
    d->csd.assign(csd, csd + csd_size);
  std::lock_guard<std::mutex> lock(d->shared->mu);
  if (!d->factory || !d->rebuild()) {
    if (error)
      *error = "no hardware decode engine";
    return nullptr;
  }
  return codec;
}

std::unique_ptr<DmaCodec> DmaCodec::configure(const std::string& mime,
                                              const std::uint8_t* csd,
                                              std::size_t csd_size,
                                              int width,
                                              int height,
                                              std::string* error) {
  const std::uint32_t fourcc = fourcc_for_mime(mime);
  if (fourcc == 0) {
    if (error)
      *error = "no hardware decode engine for " + mime;
    return nullptr;
  }
  // The V4L2 engines default to the Raspberry Pi's node numbers. Ask the
  // nodes instead; ACL_VIDEO_DEVICE names one outright.
  std::string m2m;
  std::string stateless;
  if (const char* named = std::getenv("ACL_VIDEO_DEVICE")) {
    m2m = named;
    stateless = named;
  } else {
    m2m = find_m2m_decoder(fourcc);
    if (fourcc == V4L2_PIX_FMT_HEVC)
      stateless = find_m2m_decoder(V4L2_PIX_FMT_HEVC_SLICE);
  }
  const auto w = static_cast<std::uint32_t>(std::clamp(width, 0, 16384));
  const auto h = static_cast<std::uint32_t>(std::clamp(height, 0, 16384));
  auto codec = with_factory(
      [fourcc, w, h, m2m, stateless] {
        v4l2wc::DmaDecoderConfig config;
        config.codec_fourcc = fourcc;
        config.coded_width = w;
        config.coded_height = h;
        config.v4l2_device = m2m.empty() ? nullptr : m2m.c_str();
        config.v4l2_hevc_device =
            stateless.empty() ? nullptr : stateless.c_str();
        return v4l2wc::CreateDmaDecoder(config);
      },
      csd, csd_size, error);
  if (codec != nullptr) {
    codec->d_->width = w;
    codec->d_->height = h;
    log::i(kTag, "decoder configured {} {}x{}", mime, w, h);
  }
  return codec;
}

int DmaCodec::send_packet(const std::uint8_t* data,
                          std::size_t size,
                          std::int64_t pts_us,
                          std::string* error) {
  Impl* d = d_.get();
  auto err = [&](const char* m) {
    if (error)
      *error = m;
    return -1;
  };
  std::lock_guard<std::mutex> lock(d->shared->mu);
  v4l2wc::IDmaDecoder* dec = d->shared->dec.get();
  if (dec == nullptr)
    return err("the decoder could not be rebuilt");
  // receive_frame() hands out the old decoder's last frames, replaces it and
  // sends what it had not finished. Until then nothing new goes in.
  if (d->change_pending || !d->replay.empty())
    return 1;
  if (data == nullptr || size == 0) {
    dec->Drain();
    return 0;
  }
  const std::uint8_t* unit = data;
  std::size_t unit_size = size;
  if (d->csd_due && !d->csd.empty()) {
    d->scratch.assign(d->csd.begin(), d->csd.end());
    d->scratch.insert(d->scratch.end(), data, data + size);
    unit = d->scratch.data();
    unit_size = d->scratch.size();
  }
  switch (dec->SubmitBitstream(unit, unit_size,
                               static_cast<std::uint64_t>(pts_us))) {
    case v4l2wc::SubmitResult::kOk:
      d->csd_due = false;
      d->remember(data, size, pts_us);
      return 0;
    case v4l2wc::SubmitResult::kTryAgain:
      return 1;
    case v4l2wc::SubmitResult::kSourceChange:
      d->change_pending = true;
      return 1;
    case v4l2wc::SubmitResult::kError:
      break;
  }
  return err("the decoder refused an access unit");
}

int DmaCodec::receive_frame(gfx::DmabufFrame* out,
                            std::int64_t* pts_us,
                            std::string* error) {
  Impl* d = d_.get();
  auto err = [&](const char* m) {
    if (error)
      *error = m;
    return -1;
  };
  if (out == nullptr)
    return err("null out");
  gfx::DmabufFrame frame{};
  {
    std::lock_guard<std::mutex> lock(d->shared->mu);
    Impl::Shared* s = d->shared.get();
    if (s->dec == nullptr)
      return err("the decoder could not be rebuilt");
    v4l2wc::V4l2DmaFrame f;
    bool have = s->dec->Acquire(&f);
    // The second pass is for a change of geometry found by the first: with
    // the old decoder's frames all out, its replacement is driven at once.
    for (int pass = 0; pass < 2 && !have; ++pass) {
      if (d->change_pending && !d->replace_for_change())
        return err("the decoder could not follow a change of geometry");
      if (!d->pump_replay())
        return err("the decoder refused an access unit");
      switch (s->dec->Drive()) {
        case v4l2wc::DriveResult::kOk:
          break;
        case v4l2wc::DriveResult::kSourceChange:
          d->change_pending = true;
          break;
        case v4l2wc::DriveResult::kError:
          return err("the decoder failed");
      }
      have = s->dec->Acquire(&f);
      if (!d->change_pending)
        break;
    }
    if (!have)
      return 0;
    d->forget(static_cast<std::int64_t>(f.timestamp));
    d->rebuilds_without_frame = 0;

    // One dma-buf holding every plane is what a DmabufFrame describes.
    bool sane = f.num_planes >= 1 &&
                f.num_planes <= gfx::DmabufFrame::kMaxPlanes && f.fds[0] >= 0 &&
                f.width > 0 && f.height > 0 && f.width <= 16384 &&
                f.height <= 16384;
    for (std::uint32_t i = 1; sane && i < f.num_planes; ++i)
      sane = f.fds[i] == f.fds[0];
    const off_t size = sane ? ::lseek(f.fds[0], 0, SEEK_END) : 0;
    for (std::uint32_t i = 0; sane && i < f.num_planes; ++i)
      sane = f.pitches[i] > 0 &&
             (size <= 0 || f.offsets[i] < static_cast<std::uint64_t>(size));
    const int fd = sane ? ::fcntl(f.fds[0], F_DUPFD_CLOEXEC, 0) : -1;
    if (fd < 0) {
      s->dec->Release(f.capture_index);
      return err(sane ? "out of file descriptors for a decoded frame"
                      : "a decoded frame is not one dma-buf");
    }
    frame.fd = fd;
    // An engine that reports its coded size (640x368 for a 640x360
    // picture) would show the padding. Within one coding block of the
    // configured size, the configured size is the picture.
    constexpr std::uint32_t kBlock = 64;
    const bool padded = d->width > 0 && d->height > 0 && f.width >= d->width &&
                        f.width - d->width < kBlock && f.height >= d->height &&
                        f.height - d->height < kBlock;
    frame.width = padded ? d->width : f.width;
    frame.height = padded ? d->height : f.height;
    frame.drm_format = f.drm_fourcc;
    frame.modifier = f.modifier;
    frame.modifier_tiled = f.modifier != 0;
    frame.plane_count = f.num_planes;
    for (std::uint32_t i = 0; i < f.num_planes; ++i) {
      frame.planes[i].offset = f.offsets[i];
      frame.planes[i].stride = f.pitches[i];
    }
    frame.mem_size = size > 0 ? static_cast<std::uint64_t>(size) : 0;
    auto token = std::make_shared<Impl::Token>();
    token->shared = d->shared;
    token->generation = s->generation.load();
    token->index = f.capture_index;
    token->fd = fd;
    ++s->frames_out;
    frame.hold = std::move(token);
    if (pts_us != nullptr)
      *pts_us = static_cast<std::int64_t>(f.timestamp);
    if (!d->announced) {
      d->announced = true;
      log::i(kTag,
             "frames {}x{}: the decoder's own dma-bufs, no copy ({} in "
             "its pool)",
             frame.width, frame.height, s->dec->PoolSize());
    }
  }
  // Outside the lock: this drops the caller's previous frame, whose buffer
  // goes back to the decoder under it.
  *out = std::move(frame);
  return 1;
}

void DmaCodec::flush() {
  Impl* d = d_.get();
  std::lock_guard<std::mutex> lock(d->shared->mu);
  d->in_flight.clear();
  d->in_flight_bytes = 0;
  d->replay.clear();
  d->change_pending = false;
  d->rebuilds_without_frame = 0;
  d->csd_due = true;
  if (d->shared->dec == nullptr)
    return;
  // An engine's flush takes every buffer back, so it is for when none is in
  // use. A frame on screen is still in use: the decoder is replaced, and
  // that frame keeps its memory.
  if (d->shared->frames_out == 0)
    d->shared->dec->Flush();
  else if (!d->rebuild())
    log::w(kTag, "no decoder after a flush");
}

}  // namespace acl::media

#endif  // ACL_HAVE_DMABUF_CODEC

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l fuzz harness (docs/adr/ADR-0009 C5); the libFuzzer
// entry point as documented at llvm.org/docs/LibFuzzer.html. No GPL-3.0
// sources.
//
// Fuzzes everything the --split broker does with what an app process sends:
// ipc::Channel::recv, the broker loop, the validation in wm/split_validate.h.
// The input is a script of datagrams. Each is queued on the child's end of a
// real socket pair, with descriptors of the fuzzer's choosing attached, and
// the broker then serves the socket to a presenter that stands in for the
// compositor and aborts if anything reaches it that validation should have
// stopped.
//
// Record format, repeated to the end of the input:
//   u8  descriptors: bits 0-1 how many (0-3), bits 2-3 which kinds
//       Bit 7 of the input's first byte: the child stays connected while the
//       broker serves, and the broker returns to the compositor after every
//       message. What the broker sends then arrives, so a resize it passes
//       on takes effect. Without it the child has gone before the broker
//       starts, which is the other half of what a child can do.
//   u16 length, little-endian, capped at 2048
//   that many bytes: the datagram
//
// Build and run: see tests/fuzz/meson.build.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "acl_config.h"
#include "ipc/ipc.h"
#include "log/log.h"
#include "wm/split_present.h"
#include "wm/video_geometry.h"

#if !(ACL_HAVE_VULKAN && ACL_HAVE_WCS)
#error "the broker is built only with the GPU present path (Vulkan and wcs)"
#endif

using namespace acl;

namespace {

const std::vector<std::uint64_t> kModifiers = {0x11, 0x0200000000401b03ull};

[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "broker_fuzz: %s\n", what);
  std::abort();
}

// Strict UTF-8 with no control characters: what the broker promises of a
// label, checked by other code than the broker's.
bool printable_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t len = b < 0x80         ? 1
                      : (b >> 5) == 6  ? 2
                      : (b >> 4) == 14 ? 3
                      : (b >> 3) == 30 ? 4
                                       : 0;
    if (len == 0 || i + len > s.size())
      return false;
    std::uint32_t cp = len == 1 ? b : b & (0xffu >> (len + 1));
    for (std::size_t k = 1; k < len; ++k) {
      const auto c = static_cast<unsigned char>(s[i + k]);
      if ((c & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (c & 0x3fu);
    }
    static constexpr std::uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[len] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
        cp < 0x20 || (cp >= 0x7f && cp < 0xa0))
      return false;
    i += len;
  }
  return true;
}

struct Surface {
  int w = 0, h = 0;
};

class Presenter final : public wm::IPresenter {
 public:
  explicit Presenter(Surface s) : surface_(s) {}
  int fd() noexcept override { return -1; }
  int repeat_fd() noexcept override { return -1; }
  bool dispatch() noexcept override { return true; }
  void dispatch_repeat() noexcept override {}
  bool ready() const noexcept override { return (calls_ & 1) == 0; }
  int next_buffer() const noexcept override { return 0; }
  std::uint32_t free_mask() const noexcept override { return 0b111; }
  std::uint64_t frame_time_ns() const noexcept override { return 1; }
  bool last_present_feedback(std::uint64_t* present_ns,
                             std::uint32_t* refresh_ns,
                             std::uint32_t* flags) const noexcept override {
    *present_ns = 1;
    *refresh_ns = 1;
    *flags = 0;
    return true;
  }
  const std::vector<std::uint64_t>& modifiers() const noexcept override {
    return kModifiers;
  }
  bool submit(const gfx::DmabufFrame& f, int slot) noexcept override {
    ++calls_;
    struct stat st{};
    if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode))
      fail("a frame's descriptor is not a buffer");
    // The first frame of the size the compositor asked for makes it the
    // surface's; the size left behind is then no longer one.
    if (asked_.w != 0 && static_cast<int>(f.width) == asked_.w &&
        static_cast<int>(f.height) == asked_.h) {
      surface_ = asked_;
      asked_ = {};
    }
    if (static_cast<int>(f.width) != surface_.w ||
        static_cast<int>(f.height) != surface_.h)
      fail("a frame is not the surface's size");
    if (f.drm_format != 0x34325241u && f.drm_format != 0x34325258u)
      fail("a frame has a format the presenter does not import");
    if (f.modifier != 0 && f.modifier != 0x00ffffffffffffffull &&
        std::find(kModifiers.begin(), kModifiers.end(), f.modifier) ==
            kModifiers.end())
      fail("a frame has a modifier that was not advertised");
    if (f.plane_count < 1 || f.plane_count > 4)
      fail("a frame has a bad plane count");
    const std::uint64_t end =
        f.planes[0].offset +
        static_cast<std::uint64_t>(f.planes[0].stride) * f.height;
    if (f.planes[0].stride < 4ull * f.width ||
        end > static_cast<std::uint64_t>(st.st_size))
      fail("a frame's pixels do not fit its buffer");
    for (std::uint32_t i = 1; i < f.plane_count; ++i) {
      if (f.planes[i].stride == 0 ||
          f.planes[i].offset >= static_cast<std::uint64_t>(st.st_size))
        fail("an auxiliary plane is outside the buffer");
    }
    if (slot < -1 || slot > 63)
      fail("a frame has a slot outside the ring");
    return (calls_ % 3) != 0;  // refuse some, as a busy slot would
  }
  bool keepalive() noexcept override {
    ++calls_;
    return true;
  }
  bool closed() const noexcept override { return false; }
  // The compositor asks for half the size, once, after the first frame.
  bool take_resize(int* width, int* height) noexcept override {
    if (resized_ || calls_ == 0)
      return false;
    resized_ = true;
    const Surface half{std::max(1, surface_.w / 2),
                       std::max(1, surface_.h / 2)};
    if (half.w != surface_.w || half.h != surface_.h)
      asked_ = half;
    *width = half.w;
    *height = half.h;
    return true;
  }
  // The video plane: NV12 is advertised with two modifiers, P010 is not.
  std::vector<std::uint64_t> modifiers_for(
      std::uint32_t drm_format) const noexcept override {
    if (drm_format == 0x3231564eu)
      return {0, 0x0200000000401b03ull};
    return {};
  }
  void present_video(const gfx::DmabufFrame& f,
                     int x,
                     int y,
                     int w,
                     int h) noexcept override {
    struct stat st{};
    if (::fstat(f.fd, &st) != 0 || !S_ISREG(st.st_mode))
      fail("a video frame's descriptor is not a buffer");
    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (f.width == 0 || f.height == 0 || f.width > ipc::kMaxSurfaceDim ||
        f.height > ipc::kMaxSurfaceDim)
      fail("a video frame has a size out of range");
    if (f.drm_format != 0x3231564eu)
      fail("a video frame has a format that was not advertised");
    if (f.modifier != 0 && f.modifier != 0x0200000000401b03ull)
      fail("a video frame has a modifier that was not advertised");
    if (f.plane_count < 2 || f.plane_count > 4)
      fail("a video frame has a bad plane count");
    if (f.planes[0].stride < f.width ||
        f.planes[0].offset +
                static_cast<std::uint64_t>(f.planes[0].stride) * f.height >
            size)
      fail("a video frame's luma does not fit its buffer");
    if (f.planes[1].stride < (f.width + 1) / 2 * 2 ||
        f.planes[1].offset + static_cast<std::uint64_t>(f.planes[1].stride) *
                                 ((f.height + 1) / 2) >
            size)
      fail("a video frame's chroma does not fit its buffer");
    check_rect(x, y, w, h);
    // What the real presenter does with the rectangle next.
    const wm::VideoPlacement p =
        wm::place_video(x, y, w, h, surface_.w, surface_.h, f.width, f.height);
    if (p.visible && (p.sw < 1.0 || p.sh < 1.0 || p.sx < 0 || p.sy < 0 ||
                      p.sx + p.sw > f.width || p.sy + p.sh > f.height ||
                      p.w <= 0 || p.h <= 0 || p.x < 0 || p.y < 0 ||
                      p.x + p.w > surface_.w || p.y + p.h > surface_.h))
      fail("a video frame would be placed where the compositor refuses it");
  }
  void set_video_bounds(int x, int y, int w, int h) noexcept override {
    check_rect(x, y, w, h);
  }
  void set_ime_visible(bool) noexcept override {}
  void set_clipboard(const std::string& text) noexcept override {
    if (text.size() > ipc::kMaxText)
      fail("clipboard text over the bound");
  }

 private:
  static void check_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0 || w > ipc::kMaxVideoCoord ||
        h > ipc::kMaxVideoCoord || x < -ipc::kMaxVideoCoord ||
        x > ipc::kMaxVideoCoord || y < -ipc::kMaxVideoCoord ||
        y > ipc::kMaxVideoCoord)
      fail("a video rectangle is out of range");
  }
  Surface surface_;
  Surface asked_;  // a size asked for and not yet seen in a frame
  bool resized_ = false;
  unsigned calls_ = 0;
};

// What the broker plays the child's audio through. Checks what must hold for
// anything that reaches an audio device, and reports itself part full so
// that the drop path runs too.
int g_sinks_open = 0;
class Sink final : public audio::Sink {
 public:
  ~Sink() override { close(); }
  bool open(const audio::Format& fmt) override {
    if (fmt.sample_rate < static_cast<int>(ipc::kMinAudioRate) ||
        fmt.sample_rate > static_cast<int>(ipc::kMaxAudioRate))
      fail("an audio stream has a rate out of range");
    if (fmt.channels < 1 ||
        fmt.channels > static_cast<int>(ipc::kMaxAudioChannels))
      fail("an audio stream has a channel count out of range");
    fmt_ = fmt;
    open_ = true;
    // Open, or closed by the child and still playing out: never more than
    // the child may open over a connection.
    if (++g_sinks_open > wm::BrokerOptions{}.max_audio_opens)
      fail("more audio sinks alive than the child may open");
    return (fmt.sample_rate % 7) != 0;  // some devices do not open
  }
  int write(const std::int16_t* pcm, int n_samples) override {
    if (!open_)
      fail("a write to an audio stream that is not open");
    if (n_samples <= 0 || n_samples % fmt_.channels != 0)
      fail("an audio write is not a whole number of frames");
    if (static_cast<std::size_t>(n_samples) * sizeof(std::int16_t) >
        ipc::kMaxText)
      fail("an audio write is larger than a datagram");
    // Read every sample: under ASan this is the bounds check.
    std::int32_t sum = 0;
    for (int i = 0; i < n_samples; ++i)
      sum += pcm[i];
    pending_ += static_cast<std::uint64_t>(n_samples / fmt_.channels) +
                static_cast<std::uint64_t>(sum & 1);
    return n_samples;
  }
  void drain() override {}
  void close() override {
    if (open_)
      --g_sinks_open;
    open_ = false;
  }
  std::uint64_t frames_written() const override { return 0; }
  std::uint64_t frames_pending() const override { return pending_; }
  // Any figure at all, the unknown and the absurd included, from what was
  // written: the broker bounds it before it tells the child.
  std::int64_t latency_ns() const override {
    static constexpr std::int64_t kFigures[] = {
        -1, 0, 21'000'000, 250'000'000, INT64_MAX, INT64_MIN};
    return kFigures[pending_ % 6];
  }

 private:
  audio::Format fmt_;
  bool open_ = false;
  std::uint64_t pending_ = 0;
};

wm::BufferFacts facts_of_any_file(int fd) {
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    return {};
  return {true, static_cast<std::uint64_t>(st.st_size)};
}

// Descriptors a child could send, opened once.
const std::vector<int>& descriptor_kinds() {
  static const std::vector<int> kinds = [] {
    std::vector<int> v;
    const int big = ::memfd_create("big", MFD_CLOEXEC);
    const int small = ::memfd_create("small", MFD_CLOEXEC);
    if (big < 0 || small < 0 || ::ftruncate(big, 4 << 20) != 0 ||
        ::ftruncate(small, 4096) != 0)
      fail("cannot make buffers");
    v = {big, small, ::open("/dev/null", O_RDWR | O_CLOEXEC),
         ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    return v;
  }();
  return kinds;
}

int open_descriptors() {
  int n = 0;
  for (int fd = 0; fd < 256; ++fd)
    n += ::fcntl(fd, F_GETFD) != -1;
  return n;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  // The broker logs each surface and each first refusal; at fuzzing speed
  // that is the run's whole cost. Errors still print.
  log::set_min_level(log::Level::Error);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  const std::vector<int>& kinds = descriptor_kinds();
  const int before = open_descriptors();
  {
    auto [child, broker_end] = ipc::Channel::make_pair();
    if (!child.valid())
      fail("socketpair");
    const int sock = child.native_handle();

    std::size_t at = 0;
    int queued = 0;
    while (at + 3 <= size && queued < 256) {
      const std::uint8_t sel = data[at];
      std::size_t len =
          data[at + 1] | (static_cast<std::size_t>(data[at + 2]) << 8);
      at += 3;
      len = std::min<std::size_t>({len, 2048, size - at});
      int fds[3];
      const int nfds = sel & 3;
      for (int i = 0; i < nfds; ++i)
        fds[i] = kinds[((sel >> 2) + i) % kinds.size()];
      iovec iov{const_cast<std::uint8_t*>(data + at), len};
      msghdr mh{};
      mh.msg_iov = &iov;
      mh.msg_iovlen = 1;
      alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(fds))] = {};
      if (nfds > 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        cmsghdr* cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        std::memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
      }
      at += len;
      if (::sendmsg(sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) < 0)
        break;  // the queue is full: serve what is there
      ++queued;
    }
    // Connected: end the script with a Bye, which is what makes the broker
    // return. If it does not fit, fall back to the disconnect.
    bool attached = size > 0 && (data[0] & 0x80) != 0;
    if (attached) {
      ipc::Message bye;
      bye.type = ipc::MsgType::Bye;
      child.set_send_nonblocking(true);
      attached = child.send(bye);
      queued += attached ? 1 : 0;
    }
    if (!attached)
      child.close();  // the broker reads to the end and returns

    Surface current;
    const wm::PresenterFactory open =
        [&](int w, int h, std::string_view title, std::string_view app_id,
            wm::InputCallback, bool) -> std::unique_ptr<wm::IPresenter> {
      if (w <= 0 || h <= 0 || w > static_cast<int>(ipc::kMaxSurfaceDim) ||
          h > static_cast<int>(ipc::kMaxSurfaceDim))
        fail("a surface of an impossible size");
      if (title.empty() || title.size() > 256 || !printable_utf8(title))
        fail("a title that is not fit to show");
      if (app_id.empty() || app_id.size() > 256 || !printable_utf8(app_id))
        fail("an app id that is not fit to show");
      current = {w, h};
      return std::make_unique<Presenter>(current);
    };
    wm::BrokerOptions options;
    options.inspect = facts_of_any_file;
    options.audio_sink = []() -> std::unique_ptr<audio::Sink> {
      return std::make_unique<Sink>();
    };
    if (attached)
      options.drain_budget = 1;
    const wm::BrokerStats stats =
        wm::serve_split_child(broker_end, open, options, /*linger_ms=*/0);
    // For whoever writes a seed: what the broker made of it.
    static const bool print_stats = std::getenv("A4L_FUZZ_STATS") != nullptr;
    if (print_stats)
      std::fprintf(stderr,
                   "broker_fuzz: %llu datagram(s), %llu frame(s), %llu "
                   "rejected, %llu resize(s)\n",
                   static_cast<unsigned long long>(stats.from_child),
                   static_cast<unsigned long long>(stats.frames),
                   static_cast<unsigned long long>(stats.rejected),
                   static_cast<unsigned long long>(stats.resizes));
    if (stats.from_child > static_cast<std::uint64_t>(queued))
      fail("more datagrams counted than were sent");
    if (stats.frames + stats.rejected > stats.from_child)
      fail("more outcomes than datagrams");
  }
  if (g_sinks_open != 0)
    fail("an audio stream outlived the broker");
  if (open_descriptors() != before)
    fail("a descriptor was kept");
  return 0;
}

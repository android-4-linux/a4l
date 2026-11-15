// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md). Buffer-centric
// transport between the bionic art-host process and the glibc acl process —
// SOCK_SEQPACKET + SCM_RIGHTS fd passing. Not an Android-visible interface (no
// Binder, no IDL). Low-dependency so both libc worlds link it. No GPL-3.0
// sources.
#ifndef ACL_IPC_IPC_H
#define ACL_IPC_IPC_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace acl::ipc {

// Wire message tags. The set is small and fixed on purpose (see
// docs/split-process-art.md).
enum class MsgType : std::uint32_t {
  Hello = 1,  // handshake (protocol version)
  Bye,        // orderly shutdown
  Log,        // art-host stdout/stderr forwarded to acl (extra = UTF-8 text)
  SurfaceCreate,  // art-host -> acl
  SurfaceResize,  // art-host -> acl
  PresentFrame,   // art-host -> acl (carries a dma-buf fd + optional
                  // acquire-fence fd)
  FrameReleased,  // acl -> art-host (buffer returned; release-fence fd)
  Input,          // acl -> art-host
  Lifecycle,      // both directions (resumed/paused/stopped + config)
  Vsync,          // acl -> art-host (Choreographer tick from wp_presentation)
  AudioOpen,      // child -> acl: a PCM stream begins (id, rate, channels)
  AudioWrite,     // child -> acl: samples for a stream (extra = S16LE)
  // Appended for the looper-driven present path (wm/split_present.h), so the
  // tags above keep their values.
  SurfaceReady,  // acl -> child: the surface exists (extra = u64 modifiers)
  PresentState,  // acl -> child: ready / next buffer / frame time / feedback
  Keepalive,     // child -> acl: a frame was dropped; keep the vsync loop alive
  ImeVisible,    // child -> acl: show/hide the compositor's text input (u32)
  Clipboard,     // child -> acl: put text on the clipboard (extra = UTF-8)
  // Appended for the video plane: a decoded frame goes to a subsurface below
  // the app's window, not through the app's own buffers.
  VideoFrame,   // child -> acl: a decoded frame (carries a dma-buf fd)
  VideoBounds,  // child -> acl: where the app's video View is in its surface
  VideoHide,    // child -> acl: take the picture off the video plane
  VideoReset,   // child -> acl: the screen changed; drop the plane and its hole
  AudioClose,   // child -> acl: a PCM stream ends (u32 = stream id)
};

// Sent in Hello. Bump when Message changes size or a payload changes layout:
// both ends are built from this header, and a datagram of another size is
// refused as truncated. 2: PresentFrame planes, Input keysym, PresentState.
// 3: PresentState.frames_seen. 4: the Video* messages. 5: audio as samples
// in the datagram (AudioOpen and AudioWrite changed, AudioClose added).
inline constexpr std::uint32_t kProtocolVersion = 5;

// Bounds both ends enforce on what the other sends.
inline constexpr std::uint32_t kMaxSurfaceDim = 16384;
inline constexpr std::uint32_t kMaxPlanes = 4;
inline constexpr std::uint32_t kMaxModifiers = 256;
inline constexpr std::size_t kMaxText = 60 * 1024;  // fits one datagram
// A video rectangle's origin lies within this of the surface's, and its
// sides are no longer. An app may lay its video View out larger than the
// window and partly off it (a zoom that crops), but not without limit.
inline constexpr std::int32_t kMaxVideoCoord = 4 * 16384;
// Audio: streams open at once, and what a stream may be.
inline constexpr std::uint32_t kMaxAudioStreams = 8;
inline constexpr std::uint32_t kMaxAudioChannels = 8;
inline constexpr std::uint32_t kMinAudioRate = 4000;
inline constexpr std::uint32_t kMaxAudioRate = 192000;

struct SurfaceInfo {  // Create/Resize payload
  std::uint32_t surface_id;
  std::uint32_t w, h;
  std::uint32_t want_ime;  // Create: bind the compositor's text input
  // Create: extra = title, NUL, app id.
};

struct SurfaceReady {
  std::uint32_t ok;              // 0 = the surface could not be created
  std::uint32_t modifier_count;  // extra = that many u64 DRM modifiers
};

// What the child's presenter mirrors of the real one. Sent when `ready` or
// `closed` changes, so a steady frame loop costs one datagram each way.
struct PresentState {
  std::uint32_t ready;      // the compositor wants a frame and a buffer is free
  std::uint32_t free_mask;  // free buffer slots, bit i for slot i
  std::uint32_t closed;     // the toplevel was closed
  std::uint32_t feedback_ok;  // the three fields below are valid
  std::uint64_t frame_time_ns;
  std::uint64_t present_ns;
  std::uint32_t refresh_ns;
  std::uint32_t feedback_flags;
  // Appended. How many PresentFrame messages the broker had handled when it
  // wrote this. A state written before the child's latest frame arrived says
  // nothing about that frame's slot, and the child must not read it as "the
  // slot is free": it would draw into a buffer that is on its way to the
  // screen.
  std::uint32_t frames_seen;
};

struct PresentFrame {
  std::uint32_t surface_id;
  std::uint32_t buffer_id;  // echoed back in FrameReleased
  std::uint32_t w, h;
  std::uint32_t drm_format;  // fourcc
  std::uint64_t modifier;    // DRM format modifier (tiling)
  std::uint32_t stride, offset;
  std::int32_t damage_x, damage_y, damage_w, damage_h;
  // Appended: planes 1..3 of a multi-plane buffer. plane_count 0 means 1, so
  // an older sender's single-plane frame reads correctly.
  std::uint32_t plane_count;
  std::uint32_t plane_stride[3], plane_offset[3];
  // the dma-buf fd (and, if >=0, an acquire-fence fd) travel as SCM_RIGHTS
  // ancillary data.
};

// A rectangle in the app surface's coordinates.
struct VideoRect {
  std::int32_t x, y, w, h;
};

// One decoded frame for the video plane. Each is its own dma-buf; there are
// no slots, and the broker keeps at most a few alive.
struct VideoFrame {
  std::uint32_t w, h;        // the decoded picture, not the surface
  std::uint32_t drm_format;  // fourcc: a YUV format the decoder produced
  std::uint32_t plane_count;
  std::uint64_t modifier;
  std::uint32_t stride[4], offset[4];
  VideoRect dest;  // where to show it, unless VideoBounds has said
  // the dma-buf fd travels as SCM_RIGHTS ancillary data.
};

struct FrameReleased {
  std::uint32_t surface_id;
  std::uint32_t buffer_id;
  // an optional release-fence fd travels as ancillary data.
};

struct Input {
  // 0 = pointer, 1 = key, 2 = text (extra = UTF-8), 3 = delete surrounding
  std::uint32_t kind;
  // pointer: 0 down, 1 move, 2 up. key: 0 down, 1 up. text: 1 = preedit.
  std::int32_t action;
  float x, y;            // pointer
  std::int32_t keycode;  // key: Linux evdev code. text: cursor position
  std::int64_t timestamp_ns;
  // Appended.
  std::uint32_t keysym;         // key: xkb keysym, or 0
  std::uint32_t before, after;  // delete surrounding: bytes each side
};

struct Lifecycle {
  std::uint32_t state;  // 0 stopped, 1 paused, 2 resumed
  std::uint32_t w, h, density;
};

// A PCM stream: interleaved signed 16-bit samples, as AudioTrack's
// ENCODING_PCM_16BIT. The samples travel in the datagram, not in shared
// memory: a mapping the app process can truncate is one the broker could
// fault on, and audio is small enough to copy.
struct AudioOpen {
  std::uint32_t stream;  // 0 to kMaxAudioStreams - 1, the child's choice
  std::uint32_t rate, channels;
};

struct AudioWrite {
  std::uint32_t stream;
  // extra = the samples, little-endian, a whole number of frames.
};

// One datagram = a fixed header + optional trailing bytes (in `extra`) + up to
// two ancillary fds. The union keeps the header POD and copyable; `extra_len`
// tells the receiver how much trailer followed the header inside the same
// SEQPACKET datagram.
struct Message {
  MsgType type;
  std::uint32_t extra_len;  // bytes of trailing payload after this header
  union {
    SurfaceInfo surface;
    SurfaceReady surface_ready;
    PresentState present_state;
    PresentFrame present;
    FrameReleased released;
    VideoFrame video;
    VideoRect video_rect;  // VideoBounds
    Input input;
    Lifecycle lifecycle;
    AudioOpen audio_open;
    AudioWrite audio_write;
    std::uint32_t u32;  // Hello version, etc.
  };
  // Zeroed in full: the union is larger than most payloads, and whatever is
  // left in it crosses the process boundary.
  Message() {
    std::memset(static_cast<void*>(this), 0, sizeof(*this));
    type = MsgType::Hello;
  }
};

// A received message plus whatever came with it.
struct Received {
  Message msg;
  std::vector<std::byte> extra;  // extra_len bytes
  int fd0 =
      -1;  // first ancillary fd (dma-buf / shm / release-fence), -1 if none
  int fd1 = -1;  // second ancillary fd (acquire-fence), -1 if none
};

// A SEQPACKET endpoint. Owns its fd; move-only. Errors are reported via return
// value + errno.
class Channel {
 public:
  Channel() = default;
  explicit Channel(int fd) : fd_(fd) {}
  Channel(Channel&& o) noexcept
      : fd_(o.fd_), send_nonblocking_(o.send_nonblocking_) {
    o.fd_ = -1;
  }
  Channel& operator=(Channel&& o) noexcept;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  ~Channel();

  bool valid() const { return fd_ >= 0; }
  int native_handle() const { return fd_; }
  void close();

  // Send `m` plus `extra` trailing bytes plus up to two fds, as one datagram.
  // `extra.size()` must equal `m.extra_len` (asserted). Returns false on error
  // (errno set).
  bool send(const Message& m,
            std::span<const std::byte> extra = {},
            int fd0 = -1,
            int fd1 = -1);

  // Receive one datagram. Returns false on EOF (peer closed) or error (errno
  // set; 0 on clean EOF). With `nonblock`, also returns false when nothing is
  // queued, with errno EAGAIN.
  bool recv(Received& out, bool nonblock = false);

  // Make send() fail with EAGAIN instead of blocking when the peer has
  // stopped reading and its queue is full. The broker sets this, so an app
  // process that stalls cannot stall the broker.
  void set_send_nonblocking(bool on) noexcept { send_nonblocking_ = on; }

  // A connected pair of endpoints (art-host end, acl end). Returns {invalid,
  // invalid} on failure.
  static std::pair<Channel, Channel> make_pair();

 private:
  int fd_ = -1;
  bool send_nonblocking_ = false;
};

}  // namespace acl::ipc

#endif  // ACL_IPC_IPC_H

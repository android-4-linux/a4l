// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md). Buffer-centric transport between the
// bionic art-host process and the glibc acl process — SOCK_SEQPACKET + SCM_RIGHTS fd passing. Not an
// Android-visible interface (no Binder, no IDL). Low-dependency so both libc worlds link it. No GPL-3.0 sources.
#ifndef ACL_IPC_IPC_H
#define ACL_IPC_IPC_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace acl::ipc {

// Wire message tags. The set is small and fixed on purpose (see docs/split-process-art.md).
enum class MsgType : std::uint32_t {
  Hello = 1,      // handshake (protocol version)
  Bye,            // orderly shutdown
  Log,            // art-host stdout/stderr forwarded to acl (extra = UTF-8 text)
  SurfaceCreate,  // art-host -> acl
  SurfaceResize,  // art-host -> acl
  PresentFrame,   // art-host -> acl (carries a dma-buf fd + optional acquire-fence fd)
  FrameReleased,  // acl -> art-host (buffer returned; release-fence fd)
  Input,          // acl -> art-host
  Lifecycle,      // both directions (resumed/paused/stopped + config)
  Vsync,          // acl -> art-host (Choreographer tick from wp_presentation)
  AudioOpen,      // art-host -> acl (carries a shm-ring fd)
  AudioWrite,     // art-host -> acl (ring write cursor advanced)
};

struct SurfaceInfo {  // Create/Resize payload
  std::uint32_t surface_id;
  std::uint32_t w, h;
};

struct PresentFrame {
  std::uint32_t surface_id;
  std::uint32_t buffer_id;   // echoed back in FrameReleased
  std::uint32_t w, h;
  std::uint32_t drm_format;  // fourcc
  std::uint64_t modifier;    // DRM format modifier (tiling)
  std::uint32_t stride, offset;
  std::int32_t damage_x, damage_y, damage_w, damage_h;
  // the dma-buf fd (and, if >=0, an acquire-fence fd) travel as SCM_RIGHTS ancillary data.
};

struct FrameReleased {
  std::uint32_t surface_id;
  std::uint32_t buffer_id;
  // an optional release-fence fd travels as ancillary data.
};

struct Input {
  std::uint32_t kind;    // 0 = pointer, 1 = key
  std::int32_t action;   // down/up/move (pointer) or down/up (key)
  float x, y;            // pointer
  std::int32_t keycode;  // key
  std::int64_t timestamp_ns;
};

struct Lifecycle {
  std::uint32_t state;  // 0 stopped, 1 paused, 2 resumed
  std::uint32_t w, h, density;
};

struct AudioOpen {
  std::uint32_t rate, channels, ring_bytes;
  // the shm ring fd travels as ancillary data.
};

struct AudioWrite {
  std::uint64_t write_cursor;  // total frames written to the ring
  std::uint32_t frames;        // frames in this advance
};

// One datagram = a fixed header + optional trailing bytes (in `extra`) + up to two ancillary fds.
// The union keeps the header POD and copyable; `extra_len` tells the receiver how much trailer
// followed the header inside the same SEQPACKET datagram.
struct Message {
  MsgType type;
  std::uint32_t extra_len;  // bytes of trailing payload after this header
  union {
    SurfaceInfo surface;
    PresentFrame present;
    FrameReleased released;
    Input input;
    Lifecycle lifecycle;
    AudioOpen audio_open;
    AudioWrite audio_write;
    std::uint32_t u32;  // Hello version, etc.
  };
  Message() : type(MsgType::Hello), extra_len(0), u32(0) {}
};

// A received message plus whatever came with it.
struct Received {
  Message msg;
  std::vector<std::byte> extra;  // extra_len bytes
  int fd0 = -1;                  // first ancillary fd (dma-buf / shm / release-fence), -1 if none
  int fd1 = -1;                  // second ancillary fd (acquire-fence), -1 if none
};

// A SEQPACKET endpoint. Owns its fd; move-only. Errors are reported via return value + errno.
class Channel {
 public:
  Channel() = default;
  explicit Channel(int fd) : fd_(fd) {}
  Channel(Channel&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  Channel& operator=(Channel&& o) noexcept;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  ~Channel();

  bool valid() const { return fd_ >= 0; }
  int native_handle() const { return fd_; }
  void close();

  // Send `m` plus `extra` trailing bytes plus up to two fds, as one datagram. `extra.size()` must
  // equal `m.extra_len` (asserted). Returns false on error (errno set).
  bool send(const Message& m, std::span<const std::byte> extra = {}, int fd0 = -1, int fd1 = -1);

  // Receive one datagram. Returns false on EOF (peer closed) or error (errno set; 0 on clean EOF).
  bool recv(Received& out);

  // A connected pair of endpoints (art-host end, acl end). Returns {invalid, invalid} on failure.
  static std::pair<Channel, Channel> make_pair();

 private:
  int fd_ = -1;
};

}  // namespace acl::ipc

#endif  // ACL_IPC_IPC_H

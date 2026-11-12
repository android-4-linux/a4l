// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.ImageReader semantics (SDK docs: a fixed pool of
// maxImages buffers, a producer Surface, acquireLatestImage/acquireNextImage on
// the consumer, close() to recycle) and the Android BufferQueue
// producer/consumer model (conceptual, from public docs — NOT the AOSP
// gralloc/IGraphicBufferProducer implementation). The BufferQueue-equivalent
// under acl's real android.media.ImageReader ([[flutter-texture-bridge]] M3c):
// a ring of gfx::AHardwareBuffer (dma-buf) buffers cycled producer -> consumer,
// so the engine's ImageExternalTexture can pull the latest frame as a
// HardwareBuffer. No SurfaceFlinger, no gralloc, no the predecessor.

#ifndef ACL_GFX_BUFFER_QUEUE_H
#define ACL_GFX_BUFFER_QUEUE_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gfx/ahardware_buffer.h"

namespace acl::gfx {

// A fixed pool of dma-buf buffers shared between one producer (renders frames)
// and one consumer (the engine, acquiring the latest). Each buffer is in
// exactly one state: FREE (the producer may dequeue it), READY (produced,
// waiting), or ACQUIRED (held by a consumer Image). Thread-safe: producer and
// consumer run on different threads.
class BufferQueue {
 public:
  // Allocate `count` WxH buffers of `format` (an HbFormat value). Returns
  // nullptr + *error if the buffers can't be allocated. count is clamped to
  // >=1.
  static std::unique_ptr<BufferQueue> create(std::uint32_t width,
                                             std::uint32_t height,
                                             std::uint32_t format,
                                             std::uint64_t usage,
                                             int count,
                                             std::string* error);
  ~BufferQueue();
  BufferQueue(const BufferQueue&) = delete;
  BufferQueue& operator=(const BufferQueue&) = delete;

  std::uint32_t width() const { return width_; }
  std::uint32_t height() const { return height_; }
  std::uint32_t format() const { return format_; }

  // Producer: take a FREE buffer to render into (nullptr if all are in flight).
  // The returned handle stays owned by the queue; do not release it. queue() it
  // when the frame is ready.
  AHardwareBuffer* dequeue();
  // Producer: mark a dequeued buffer READY (a new frame). Fires the
  // frame-available callback (which the ImageReader JNI maps to
  // onImageAvailable). No-op if the buffer isn't one of ours / not dequeued.
  void queue(AHardwareBuffer* buffer);

  // Producer (ZERO-COPY decode path): queue an EXTERNALLY-imported buffer (a
  // hardware decoder's DRM_PRIME frame wrapped via ahb_import) as READY,
  // transferring the caller's ref to the queue. Unlike a pooled buffer, it is
  // not reused: once acquired-and-released (or superseded by a newer frame) the
  // queue drops its ref and removes the slot. Fires the frame-available
  // callback. This is how MediaCodec setVideoSurface feeds decoded frames — the
  // decoder owns the buffer pool, acl just routes each frame's dma-buf to the
  // consumer with no copy.
  void queue_imported(AHardwareBuffer* imported);

  // Consumer: take the most-recently-queued buffer
  // (ImageReader.acquireLatestImage) — older READY buffers are recycled to
  // FREE. Returns nullptr if nothing is ready. The buffer is ACQUIRED until
  // release()d.
  AHardwareBuffer* acquire_latest();
  // Consumer: return an acquired buffer to FREE.
  void release(AHardwareBuffer* buffer);

  // Called (on the producer thread) when a buffer becomes READY. Set by the
  // ImageReader JNI.
  void set_on_frame_available(std::function<void()> cb);

 private:
  BufferQueue() = default;

  enum class State { Free, Ready, Acquired };
  struct Slot {
    AHardwareBuffer* buf = nullptr;  // owned (refcount held by the queue)
    State state = State::Free;
    std::uint64_t seq = 0;  // queue order, to find the latest READY
    bool imported = false;  // queue_imported: a transient decoder buffer,
                            // dropped (not pooled) after use
  };

  std::mutex mtx_;
  std::vector<Slot> slots_;
  std::uint64_t next_seq_ = 1;
  std::function<void()> on_frame_available_;
  std::uint32_t width_ = 0, height_ = 0, format_ = 0;
};

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
#endif  // ACL_GFX_BUFFER_QUEUE_H

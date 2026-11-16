// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.ImageReader / BufferQueue semantics (public SDK
// docs — pool, produce, acquire-latest, recycle). Implementation is
// acl-original over gfx::AHardwareBuffer. No GPL-3.0 sources.

#include "gfx/buffer_queue.h"

#if ACL_HAVE_GL

#include <algorithm>
#include <utility>

#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "gfx.bufqueue";
}

std::unique_ptr<BufferQueue> BufferQueue::create(std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::uint32_t format,
                                                 std::uint64_t usage,
                                                 int count,
                                                 std::string* error) {
  if (count < 1)
    count = 1;
  auto q = std::unique_ptr<BufferQueue>(new BufferQueue());
  q->width_ = width;
  q->height_ = height;
  q->format_ = format;
  q->usage_ = usage;
  // The first buffer now, so that a size or format that cannot be allocated
  // fails here; the rest when a producer first asks for them. An app makes
  // its ImageReader on its main thread, and a player's is 1x1 with seven
  // buffers it never draws into: allocating them all held that thread for
  // 12 ms (measured), with the player's setVideoSurface() waiting behind it.
  q->slots_.resize(static_cast<std::size_t>(count));
  q->slots_[0].buf = q->allocate();
  if (q->slots_[0].buf == nullptr) {
    if (error)
      *error = "BufferQueue: ahb_allocate failed";
    return nullptr;
  }
  log::d(kTag, "create {}x{} format={} count={}", width, height, format, count);
  return q;
}

AHardwareBuffer* BufferQueue::allocate() const {
  ::AHardwareBuffer_Desc d{};
  d.width = width_;
  d.height = height_;
  d.layers = 1;
  d.format = format_;
  d.usage = usage_;
  return ahb_allocate(d);
}

BufferQueue::~BufferQueue() {
  for (auto& s : slots_)
    if (s.buf != nullptr)
      ahb_release(s.buf);
}

AHardwareBuffer* BufferQueue::dequeue() {
  std::lock_guard<std::mutex> lk(mtx_);
  // A free buffer that exists, else a free slot that has none yet.
  Slot* pick = nullptr;
  for (auto& s : slots_) {
    if (s.state != State::Free)
      continue;
    if (s.buf != nullptr) {
      pick = &s;
      break;
    }
    if (pick == nullptr)
      pick = &s;
  }
  if (pick == nullptr)
    return nullptr;  // all in flight
  if (pick->buf == nullptr && (pick->buf = allocate()) == nullptr)
    return nullptr;  // could not allocate; the slot stays free for a retry
  pick->state = State::Acquired;  // "owned by producer" — distinct from
                                  // FREE/READY; released by queue()
  pick->seq = 0;
  return pick->buf;
}

void BufferQueue::queue(AHardwareBuffer* buffer) {
  if (buffer == nullptr)
    return;  // would match a slot that has no buffer yet
  std::function<void()> cb;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : slots_) {
      if (s.buf == buffer) {
        s.state = State::Ready;
        s.seq = next_seq_++;
        cb = on_frame_available_;
        break;
      }
    }
  }
  if (cb)
    cb();  // outside the lock: the listener may re-enter (acquire) on another
           // thread
}

void BufferQueue::queue_imported(AHardwareBuffer* imported) {
  if (imported == nullptr)
    return;
  std::function<void()> cb;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    slots_.push_back(Slot{imported, State::Ready, next_seq_++,
                          /*imported=*/true});  // takes the ref
    cb = on_frame_available_;
  }
  if (cb)
    cb();  // outside the lock: the listener may re-enter (acquire) on another
           // thread
}

AHardwareBuffer* BufferQueue::acquire_latest() {
  std::lock_guard<std::mutex> lk(mtx_);
  Slot* latest = nullptr;
  for (auto& s : slots_)
    if (s.state == State::Ready && (latest == nullptr || s.seq > latest->seq))
      latest = &s;
  if (latest == nullptr)
    return nullptr;
  latest->state = State::Acquired;
  AHardwareBuffer* result = latest->buf;
  // Recycle older READY buffers (acquireLatestImage drops the backlog): a
  // pooled buffer returns to FREE; an imported (decoder) buffer is transient,
  // so drop the queue's ref and empty its slot.
  for (auto& s : slots_) {
    if (s.state == State::Ready) {  // latest is now Acquired, so excluded
      if (s.imported) {
        ahb_release(s.buf);
        s.buf = nullptr;
      } else
        s.state = State::Free;
    }
  }
  slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                              [](const Slot& s) {
                                // An emptied imported slot, not a pooled one
                                // whose buffer is not allocated yet.
                                return s.imported && s.buf == nullptr;
                              }),
               slots_.end());
  return result;
}

void BufferQueue::release(AHardwareBuffer* buffer) {
  if (buffer == nullptr)
    return;
  std::lock_guard<std::mutex> lk(mtx_);
  for (auto it = slots_.begin(); it != slots_.end(); ++it) {
    if (it->buf == buffer && it->state == State::Acquired) {
      if (it->imported) {
        ahb_release(it->buf);  // drop the queue's ref (any consumer
                               // HardwareBuffer keeps its own)
        slots_.erase(it);      // imported buffers aren't reused
      } else {
        it->state = State::Free;
        it->seq = 0;
      }
      return;
    }
  }
}

void BufferQueue::set_on_frame_available(std::function<void()> cb) {
  std::lock_guard<std::mutex> lk(mtx_);
  on_frame_available_ = std::move(cb);
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

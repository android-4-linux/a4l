// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; the contract of
// android.media.MediaCodec.releaseOutputBuffer(index, renderTimestampNs) as the
// SDK documents it. No GPL-3.0 sources.
//
// Do something at a time, in time order, on one thread.
//
// A player releases a video frame for display with the time it is to be
// shown, and releases it early: media3 does so up to 50 ms ahead. On Android
// the compositor holds the buffer until that time. Shown at once instead, the
// picture runs ahead of the sound by however early the player was. This is
// the holding: an action per frame, run when its time comes.
#ifndef ACL_MEDIA_TIMED_RELEASE_H
#define ACL_MEDIA_TIMED_RELEASE_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace acl::media {

class TimedRelease {
 public:
  // Called once: with true when the time has come, with false if the action
  // is cancelled first. Either way it owns what it captured.
  using Action = std::function<void(bool due)>;

  // The longest an action is held. A time further off than this is not a
  // display time; it is run after this long.
  static constexpr std::int64_t kMaxHoldNs = 200'000'000;

  TimedRelease() = default;
  ~TimedRelease() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
      thread_.join();
    cancel_all();
  }
  TimedRelease(const TimedRelease&) = delete;
  TimedRelease& operator=(const TimedRelease&) = delete;

  // CLOCK_MONOTONIC in nanoseconds: what System.nanoTime() counts on Linux.
  static std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // Run `action` at `when_ns`. A time that has passed, or 0, means now; it
  // still goes behind actions already waiting for an earlier or equal time,
  // so that frames keep their order.
  void at(std::int64_t when_ns, Action action) {
    const std::int64_t now = now_ns();
    if (when_ns < now)
      when_ns = now;
    if (when_ns > now + kMaxHoldNs)
      when_ns = now + kMaxHoldNs;
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_.emplace(when_ns, std::move(action));
      if (!thread_.joinable())
        thread_ = std::thread([this] { run(); });
    }
    cv_.notify_all();
  }

  // Run everything waiting now, in time order, on the calling thread: each
  // action is called with true. For frames a player has released when their
  // codec goes: on Android they are already the surface's, and shown.
  void release_all() {
    std::multimap<std::int64_t, Action> now;
    {
      std::lock_guard<std::mutex> lock(mu_);
      now.swap(pending_);
    }
    for (auto& [when, action] : now)
      action(true);
  }
  // Let what is waiting run at its time, waiting up to `max_wait_ns` for the
  // last of it and for an action already running; whatever is still waiting
  // then runs now, as release_all. For a codec that goes: its frames are
  // shown at their times, and none is running when it is gone.
  void finish(std::int64_t max_wait_ns) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds(max_wait_ns);
      idle_cv_.wait_until(lock, until,
                          [this] { return pending_.empty() && !running_; });
    }
    release_all();
  }
  // Cancel everything waiting. Each action is called with false.
  void cancel_all() {
    std::multimap<std::int64_t, Action> gone;
    {
      std::lock_guard<std::mutex> lock(mu_);
      gone.swap(pending_);
    }
    for (auto& [when, action] : gone)
      action(false);
  }

  std::size_t waiting() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
  }

 private:
  void run() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!quit_) {
      if (pending_.empty()) {
        cv_.wait(lock);
        continue;
      }
      const std::int64_t when = pending_.begin()->first;
      const std::int64_t now = now_ns();
      if (when > now) {
        cv_.wait_for(lock, std::chrono::nanoseconds(when - now));
        continue;  // look again: an earlier one may have arrived, or a cancel
      }
      Action action = std::move(pending_.begin()->second);
      pending_.erase(pending_.begin());
      running_ = true;
      lock.unlock();
      action(true);
      action = nullptr;  // what it owns goes before finish() is told
      lock.lock();
      running_ = false;
      if (pending_.empty())
        idle_cv_.notify_all();
    }
  }

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;  // nothing waiting, nothing running
  bool running_ = false;
  std::multimap<std::int64_t, Action>
      pending_;  // by time; equal times in order
  bool quit_ = false;
  std::thread thread_;
};

}  // namespace acl::media

#endif  // ACL_MEDIA_TIMED_RELEASE_H

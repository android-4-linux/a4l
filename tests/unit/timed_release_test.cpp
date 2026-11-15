// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test. No GPL-3.0 sources.
//
// media::TimedRelease: what holds a video frame until the time the player
// released it for. Actions run at their time and in time order, a time that
// has passed runs at once but not ahead of what is already waiting, a cancel
// tells each action it was cancelled, and nothing is lost when the object
// goes away.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "media/timed_release.h"

using acl::media::TimedRelease;

namespace {

constexpr std::int64_t kMs = 1'000'000;

struct Ran {
  std::mutex mu;
  std::vector<int> ids;  // in the order they ran
  std::vector<bool> due;
  std::vector<std::int64_t> late_ns;  // how long after its time each ran
  void add(int id, bool was_due, std::int64_t when) {
    std::lock_guard<std::mutex> lock(mu);
    ids.push_back(id);
    due.push_back(was_due);
    late_ns.push_back(TimedRelease::now_ns() - when);
  }
  std::size_t count() {
    std::lock_guard<std::mutex> lock(mu);
    return ids.size();
  }
};

bool eventually(int ms, const std::function<bool()>& done) {
  for (int i = 0; i < ms / 5; ++i) {
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

}  // namespace

TEST_CASE("timed release: an action runs at its time, not before",
          "[media][timed]") {
  Ran ran;
  TimedRelease t;
  const std::int64_t when = TimedRelease::now_ns() + 60 * kMs;
  t.at(when, [&](bool due) { ran.add(1, due, when); });
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  CHECK(ran.count() == 0);
  CHECK(t.waiting() == 1);
  REQUIRE(eventually(1000, [&] { return ran.count() == 1; }));
  CHECK(ran.due[0]);
  CHECK(ran.late_ns[0] >= 0);         // never early
  CHECK(ran.late_ns[0] < 200 * kMs);  // and not long after
  CHECK(t.waiting() == 0);
}

TEST_CASE("timed release: actions run in time order, whatever order they came",
          "[media][timed]") {
  Ran ran;
  TimedRelease t;
  const std::int64_t base = TimedRelease::now_ns() + 40 * kMs;
  // Frames released out of time order, and two for the same time.
  const std::int64_t times[] = {base + 30 * kMs, base, base + 10 * kMs,
                                base + 10 * kMs, base + 20 * kMs};
  for (int i = 0; i < 5; ++i) {
    const std::int64_t when = times[i];
    t.at(when, [&ran, i, when](bool due) { ran.add(i, due, when); });
  }
  REQUIRE(eventually(2000, [&] { return ran.count() == 5; }));
  // By time; the two with one time in the order they were given.
  CHECK(ran.ids == std::vector<int>{1, 2, 3, 4, 0});
  for (std::int64_t late : ran.late_ns)
    CHECK(late >= 0);
}

TEST_CASE("timed release: a time that has passed, or none, means now",
          "[media][timed]") {
  Ran ran;
  TimedRelease t;
  const std::int64_t now = TimedRelease::now_ns();
  t.at(0, [&](bool due) { ran.add(1, due, now); });
  t.at(now - 5'000'000'000, [&](bool due) { ran.add(2, due, now); });
  REQUIRE(eventually(500, [&] { return ran.count() == 2; }));
  CHECK(ran.ids == std::vector<int>{1, 2});  // in the order given
  CHECK(ran.due == std::vector<bool>{true, true});

  // "Now" does not jump ahead of a frame already waiting for an earlier time.
  Ran order;
  const std::int64_t soon = TimedRelease::now_ns() + 30 * kMs;
  t.at(soon, [&](bool due) { order.add(1, due, soon); });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  t.at(0, [&](bool due) { order.add(2, due, soon); });
  REQUIRE(eventually(500, [&] { return order.count() == 2; }));
  CHECK(order.ids == std::vector<int>{1, 2});
}

TEST_CASE("timed release: a time far off is not waited for", "[media][timed]") {
  Ran ran;
  TimedRelease t;
  const std::int64_t when =
      TimedRelease::now_ns() + 3'600'000'000'000;  // an hour
  const auto t0 = std::chrono::steady_clock::now();
  t.at(when, [&](bool due) { ran.add(1, due, when); });
  REQUIRE(eventually(2000, [&] { return ran.count() == 1; }));
  const auto took = std::chrono::steady_clock::now() - t0;
  CHECK(took >= std::chrono::nanoseconds(TimedRelease::kMaxHoldNs) -
                    std::chrono::milliseconds(20));
  CHECK(took < std::chrono::seconds(2));
  CHECK(ran.due[0]);
}

TEST_CASE("timed release: cancel tells each action, and each runs once",
          "[media][timed]") {
  Ran ran;
  auto t = std::make_unique<TimedRelease>();
  const std::int64_t when = TimedRelease::now_ns() + 150 * kMs;
  // What an action owns (a frame) is released either way, exactly once.
  auto owned = std::make_shared<int>(7);
  for (int i = 0; i < 3; ++i)
    t->at(when + i,
          [&ran, i, when, owned](bool due) { ran.add(i, due, when); });
  CHECK(owned.use_count() == 4);
  t->cancel_all();
  CHECK(ran.count() == 3);
  CHECK(ran.due == std::vector<bool>{false, false, false});
  CHECK(t->waiting() == 0);
  CHECK(owned.use_count() == 1);
  // Nothing runs later.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(ran.count() == 3);

  // Destroying it cancels what is waiting.
  for (int i = 0; i < 2; ++i)
    t->at(TimedRelease::now_ns() + 500 * kMs,
          [&ran, i, when, owned](bool due) { ran.add(10 + i, due, when); });
  t.reset();
  CHECK(ran.count() == 5);
  CHECK_FALSE(ran.due[3]);
  CHECK_FALSE(ran.due[4]);
  CHECK(owned.use_count() == 1);
}

TEST_CASE("timed release: it is used from many threads", "[media][timed]") {
  std::atomic<int> ran{0};
  {
    TimedRelease t;
    std::vector<std::thread> threads;
    for (int k = 0; k < 4; ++k) {
      threads.emplace_back([&t, &ran] {
        for (int i = 0; i < 200; ++i)
          t.at(TimedRelease::now_ns() + (i % 20) * kMs,
               [&ran](bool) { ++ran; });
      });
    }
    for (auto& th : threads)
      th.join();
    t.cancel_all();
    for (int i = 0; i < 50; ++i)
      t.at(0, [&ran](bool) { ++ran; });
  }
  CHECK(ran == 850);  // each exactly once, run or cancelled
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/log.md (pending), ihs_shared facade design, fmt docs. No
// GPL-3.0 sources.

#include "log/log.h"

#include <atomic>
#include <mutex>

#include <fmt/format.h>

namespace acl::log {
namespace {

// Default backend: one line per record on stderr, "<L>/<tag>: <message>".
class StderrSink final : public Sink {
 public:
  void write(Level level,
             std::string_view tag,
             std::string_view message) noexcept override {
    // fmt::print to stderr; swallow any I/O error — logging must never throw.
    try {
      fmt::print(stderr, "{}/{}: {}\n", level_char(level), tag, message);
    } catch (...) {
    }
  }
};

// The active sink is guarded by a mutex (set is rare; read is per-record but
// cheap). The minimum level is a lock-free atomic on the hot path.
std::mutex& sink_mutex() {
  static std::mutex m;
  return m;
}

std::shared_ptr<Sink>& sink_slot() {
  static std::shared_ptr<Sink> s = std::make_shared<StderrSink>();
  return s;
}

std::atomic<int>& level_slot() {
  static std::atomic<int> lvl{static_cast<int>(Level::Verbose)};
  return lvl;
}

}  // namespace

char level_char(Level level) noexcept {
  switch (level) {
    case Level::Verbose:
      return 'V';
    case Level::Debug:
      return 'D';
    case Level::Info:
      return 'I';
    case Level::Warn:
      return 'W';
    case Level::Error:
      return 'E';
    case Level::Fatal:
      return 'F';
  }
  return '?';
}

std::shared_ptr<Sink> set_sink(std::shared_ptr<Sink> sink) {
  std::lock_guard<std::mutex> lock(sink_mutex());
  std::shared_ptr<Sink> previous = sink_slot();
  sink_slot() = sink ? std::move(sink) : std::make_shared<StderrSink>();
  return previous;
}

std::shared_ptr<Sink> sink() {
  std::lock_guard<std::mutex> lock(sink_mutex());
  return sink_slot();
}

void set_min_level(Level level) noexcept {
  level_slot().store(static_cast<int>(level), std::memory_order_relaxed);
}

Level min_level() noexcept {
  return static_cast<Level>(level_slot().load(std::memory_order_relaxed));
}

void write(Level level,
           std::string_view tag,
           std::string_view message) noexcept {
  if (static_cast<int>(level) < level_slot().load(std::memory_order_relaxed)) {
    return;
  }
  std::shared_ptr<Sink> active;
  {
    std::lock_guard<std::mutex> lock(sink_mutex());
    active = sink_slot();
  }
  if (active)
    active->write(level, tag, message);
}

}  // namespace acl::log

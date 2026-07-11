// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/log.md (pending), Android SDK android.util.Log priority model,
// ihs_shared three-layer facade design. No GPL-3.0 sources.
#ifndef ACL_LOG_LOG_H
#define ACL_LOG_LOG_H

// Three-layer logging facade (public API -> dispatch -> pluggable backend sink),
// following the ihs_shared design. Priorities match android.util.Log so the
// framework's Log JNI can forward straight through in Phase 2+.

#include <memory>
#include <string_view>
#include <utility>

#include <fmt/core.h>

namespace acl::log {

// Values match android.util.Log priority constants exactly.
enum class Level : int {
  Verbose = 2,
  Debug = 3,
  Info = 4,
  Warn = 5,
  Error = 6,
  Fatal = 7,  // android.util.Log.ASSERT / Log.wtf
};

// Single character used by the default sink (V/D/I/W/E/F).
char level_char(Level) noexcept;

// Backend layer. Implement this to route logs to spdlog, DLT, logcat-style ring
// buffers, a test capture, etc. Implementations must be safe to call from any
// thread and must not throw.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void write(Level level, std::string_view tag,
                     std::string_view message) noexcept = 0;
};

// Install the active sink; returns the previously installed one. Thread-safe.
// Passing nullptr restores the default stderr sink.
std::shared_ptr<Sink> set_sink(std::shared_ptr<Sink> sink);
std::shared_ptr<Sink> sink();

// Messages below the minimum level are dropped before formatting. Thread-safe.
void set_min_level(Level level) noexcept;
Level min_level() noexcept;

// Dispatch layer. Pre-formatted; never throws.
void write(Level level, std::string_view tag, std::string_view message) noexcept;

// Public API. Formatting is skipped entirely when level < min_level().
template <typename... Args>
void logf(Level level, std::string_view tag,
          fmt::format_string<Args...> format, Args&&... args) {
  if (static_cast<int>(level) < static_cast<int>(min_level())) return;
  // fmt::format may throw only on a bad format string, which is a programming
  // error caught at build time via the compile-time checked format_string.
  write(level, tag, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void v(std::string_view tag, fmt::format_string<Args...> f, Args&&... a) {
  logf(Level::Verbose, tag, f, std::forward<Args>(a)...);
}
template <typename... Args>
void d(std::string_view tag, fmt::format_string<Args...> f, Args&&... a) {
  logf(Level::Debug, tag, f, std::forward<Args>(a)...);
}
template <typename... Args>
void i(std::string_view tag, fmt::format_string<Args...> f, Args&&... a) {
  logf(Level::Info, tag, f, std::forward<Args>(a)...);
}
template <typename... Args>
void w(std::string_view tag, fmt::format_string<Args...> f, Args&&... a) {
  logf(Level::Warn, tag, f, std::forward<Args>(a)...);
}
template <typename... Args>
void e(std::string_view tag, fmt::format_string<Args...> f, Args&&... a) {
  logf(Level::Error, tag, f, std::forward<Args>(a)...);
}

}  // namespace acl::log

#endif  // ACL_LOG_LOG_H

// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "log/log.h"

namespace {

struct Record {
  acl::log::Level level;
  std::string tag;
  std::string message;
};

class CaptureSink final : public acl::log::Sink {
 public:
  std::vector<Record> records;
  void write(acl::log::Level level,
             std::string_view tag,
             std::string_view message) noexcept override {
    records.push_back({level, std::string(tag), std::string(message)});
  }
};

// RAII guard so a failing assertion still restores the global sink/level.
class SinkGuard {
 public:
  explicit SinkGuard(std::shared_ptr<acl::log::Sink> s)
      : previous_(acl::log::set_sink(std::move(s))),
        previous_level_(acl::log::min_level()) {}
  ~SinkGuard() {
    acl::log::set_sink(previous_);
    acl::log::set_min_level(previous_level_);
  }

 private:
  std::shared_ptr<acl::log::Sink> previous_;
  acl::log::Level previous_level_;
};

}  // namespace

using acl::log::Level;

TEST_CASE("log dispatches formatted records to the active sink", "[log]") {
  auto cap = std::make_shared<CaptureSink>();
  SinkGuard guard(cap);
  acl::log::set_min_level(Level::Verbose);

  acl::log::i("Test", "hello {} {}", 1, 2);
  acl::log::e("Test", "boom");

  REQUIRE(cap->records.size() == 2);
  CHECK(cap->records[0].level == Level::Info);
  CHECK(cap->records[0].tag == "Test");
  CHECK(cap->records[0].message == "hello 1 2");
  CHECK(cap->records[1].level == Level::Error);
  CHECK(cap->records[1].message == "boom");
}

TEST_CASE("log drops records below the minimum level before formatting",
          "[log]") {
  auto cap = std::make_shared<CaptureSink>();
  SinkGuard guard(cap);
  acl::log::set_min_level(Level::Warn);

  acl::log::i("T", "dropped");
  acl::log::d("T", "dropped");
  acl::log::w("T", "kept");

  REQUIRE(cap->records.size() == 1);
  CHECK(cap->records[0].level == Level::Warn);
  CHECK(cap->records[0].message == "kept");
}

TEST_CASE("log level characters match android.util.Log priorities", "[log]") {
  CHECK(acl::log::level_char(Level::Verbose) == 'V');
  CHECK(acl::log::level_char(Level::Info) == 'I');
  CHECK(acl::log::level_char(Level::Fatal) == 'F');
  CHECK(static_cast<int>(Level::Info) == 4);   // android.util.Log.INFO
  CHECK(static_cast<int>(Level::Error) == 6);  // android.util.Log.ERROR
}

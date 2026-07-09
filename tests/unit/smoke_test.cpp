// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.
//
// Phase 0 smoke test: proves the native-core module link graph resolves. Real
// per-subsystem unit tests replace this as each module gains behavior.

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "aclcxx_config.h"

namespace aclcxx::runtime { const char* module_name() noexcept; }
namespace aclcxx::pkg     { const char* module_name() noexcept; }
namespace aclcxx::gfx     { const char* module_name() noexcept; }
namespace aclcxx::wm      { const char* module_name() noexcept; }
namespace aclcxx::input   { const char* module_name() noexcept; }
namespace aclcxx::audio   { const char* module_name() noexcept; }
namespace aclcxx::media   { const char* module_name() noexcept; }
namespace aclcxx::svc     { const char* module_name() noexcept; }
namespace aclcxx::ndk     { const char* module_name() noexcept; }

TEST_CASE("native-core modules link and report their names", "[smoke]") {
  CHECK(std::strcmp(aclcxx::runtime::module_name(), "runtime") == 0);
  CHECK(std::strcmp(aclcxx::pkg::module_name(), "pkg") == 0);
  CHECK(std::strcmp(aclcxx::gfx::module_name(), "gfx") == 0);
  CHECK(std::strcmp(aclcxx::wm::module_name(), "wm") == 0);
  CHECK(std::strcmp(aclcxx::input::module_name(), "input") == 0);
  CHECK(std::strcmp(aclcxx::audio::module_name(), "audio") == 0);
  CHECK(std::strcmp(aclcxx::media::module_name(), "media") == 0);
  CHECK(std::strcmp(aclcxx::svc::module_name(), "svc") == 0);
  CHECK(std::strcmp(aclcxx::ndk::module_name(), "ndk") == 0);
}

#if ACLCXX_PROFILE_AUTOMOTIVE
namespace aclcxx::svc::car     { const char* module_name() noexcept; }
namespace aclcxx::hosts::cal   { const char* module_name() noexcept; }
namespace aclcxx::hosts::media { const char* module_name() noexcept; }

TEST_CASE("automotive-profile modules link when the profile is ON", "[smoke][automotive]") {
  CHECK(std::strcmp(aclcxx::svc::car::module_name(), "svc/car") == 0);
  CHECK(std::strcmp(aclcxx::hosts::cal::module_name(), "hosts/cal") == 0);
  CHECK(std::strcmp(aclcxx::hosts::media::module_name(), "hosts/media") == 0);
}
#endif

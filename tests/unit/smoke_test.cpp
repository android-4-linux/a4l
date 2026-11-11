// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.
//
// Phase 0 smoke test: proves the native-core module link graph resolves. Real
// per-subsystem unit tests replace this as each module gains behavior.

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "acl_config.h"

namespace acl::runtime {
const char* module_name() noexcept;
}
namespace acl::pkg {
const char* module_name() noexcept;
}
namespace acl::gfx {
const char* module_name() noexcept;
}
namespace acl::wm {
const char* module_name() noexcept;
}
namespace acl::input {
const char* module_name() noexcept;
}
namespace acl::audio {
const char* module_name() noexcept;
}
namespace acl::media {
const char* module_name() noexcept;
}
namespace acl::svc {
const char* module_name() noexcept;
}
namespace acl::ndk {
const char* module_name() noexcept;
}

TEST_CASE("native-core modules link and report their names", "[smoke]") {
  CHECK(std::strcmp(acl::runtime::module_name(), "runtime") == 0);
  CHECK(std::strcmp(acl::pkg::module_name(), "pkg") == 0);
  CHECK(std::strcmp(acl::gfx::module_name(), "gfx") == 0);
  CHECK(std::strcmp(acl::wm::module_name(), "wm") == 0);
  CHECK(std::strcmp(acl::input::module_name(), "input") == 0);
  CHECK(std::strcmp(acl::audio::module_name(), "audio") == 0);
  CHECK(std::strcmp(acl::media::module_name(), "media") == 0);
  CHECK(std::strcmp(acl::svc::module_name(), "svc") == 0);
  CHECK(std::strcmp(acl::ndk::module_name(), "ndk") == 0);
}

#if ACL_PROFILE_AUTOMOTIVE
namespace acl::svc::car {
const char* module_name() noexcept;
}
namespace acl::hosts::cal {
const char* module_name() noexcept;
}
namespace acl::hosts::media {
const char* module_name() noexcept;
}

TEST_CASE("automotive-profile modules link when the profile is ON",
          "[smoke][automotive]") {
  CHECK(std::strcmp(acl::svc::car::module_name(), "svc/car") == 0);
  CHECK(std::strcmp(acl::hosts::cal::module_name(), "hosts/cal") == 0);
  CHECK(std::strcmp(acl::hosts::media::module_name(), "hosts/media") == 0);
}
#endif

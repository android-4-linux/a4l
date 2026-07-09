// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/shell.md (pending), Android SDK launch semantics, xdg-desktop specs. No GPL-3.0 sources.
//
// Phase 0 entrypoint. Exercises the module link graph so the skeleton is a real,
// runnable binary from the first commit; the actual `aclcxx run app.apk` launcher
// lands in Phase 1 (Gate G1).

#include <cstdio>
#include <initializer_list>

#include "aclcxx_config.h"

// Link-symbol probes exported by each native-core module. These prove the whole
// static-library graph links; they are replaced by real subsystem entrypoints.
namespace aclcxx::runtime { const char* module_name() noexcept; }
namespace aclcxx::pkg     { const char* module_name() noexcept; }
namespace aclcxx::gfx     { const char* module_name() noexcept; }
namespace aclcxx::wm      { const char* module_name() noexcept; }
namespace aclcxx::input   { const char* module_name() noexcept; }
namespace aclcxx::audio   { const char* module_name() noexcept; }
namespace aclcxx::media   { const char* module_name() noexcept; }
namespace aclcxx::svc     { const char* module_name() noexcept; }
namespace aclcxx::ndk     { const char* module_name() noexcept; }
#if ACLCXX_PROFILE_AUTOMOTIVE
namespace aclcxx::svc::car { const char* module_name() noexcept; }
#endif

int main() {
  std::printf("aclcxx %s\n", ACLCXX_VERSION);
  std::printf("profile: %s\n",
              ACLCXX_PROFILE_AUTOMOTIVE ? "automotive" : "phone");
  std::printf("modules linked:\n");
  for (const char* name : {
         aclcxx::runtime::module_name(),
         aclcxx::pkg::module_name(),
         aclcxx::gfx::module_name(),
         aclcxx::wm::module_name(),
         aclcxx::input::module_name(),
         aclcxx::audio::module_name(),
         aclcxx::media::module_name(),
         aclcxx::svc::module_name(),
         aclcxx::ndk::module_name(),
#if ACLCXX_PROFILE_AUTOMOTIVE
         aclcxx::svc::car::module_name(),
#endif
       }) {
    std::printf("  - %s\n", name);
  }
  return 0;
}

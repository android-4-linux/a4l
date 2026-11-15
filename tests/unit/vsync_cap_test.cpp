// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test (looper-driven present). No GPL-3.0 sources.
//
// The vsync rate cap: an early vsync is given a wait, never dropped. The
// defect this guards against stopped the present loop on a compositor that
// answers a frame callback at once (sway), after two frames.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "runtime/vsync_cap.h"

using acl::runtime::vsync_cap_wait_ns;
using acl::runtime::vsync_min_gap_ns;

namespace {
constexpr std::uint64_t k60 = 16'666'667ull;
constexpr std::uint64_t kT0 = 5'000'000'000ull;
}  // namespace

TEST_CASE("vsync cap: the first vsync is delivered at once", "[vsync]") {
  CHECK(vsync_cap_wait_ns(kT0, 0, k60) == 0);
  CHECK(vsync_cap_wait_ns(kT0, 0, 0) == 0);
}

TEST_CASE("vsync cap: an early vsync gets the rest of the gap", "[vsync]") {
  const std::uint64_t gap = vsync_min_gap_ns(k60);
  CHECK(gap == k60 - k60 / 8);
  // Same instant as the last delivery: the whole gap is left.
  CHECK(vsync_cap_wait_ns(kT0, kT0, k60) == gap);
  // 1 ms later: 1 ms less.
  CHECK(vsync_cap_wait_ns(kT0 + 1'000'000, kT0, k60) == gap - 1'000'000);
  // One nanosecond short of the gap.
  CHECK(vsync_cap_wait_ns(kT0 + gap - 1, kT0, k60) == 1);
}

TEST_CASE("vsync cap: a vsync at or past the gap is delivered", "[vsync]") {
  const std::uint64_t gap = vsync_min_gap_ns(k60);
  CHECK(vsync_cap_wait_ns(kT0 + gap, kT0, k60) == 0);
  // A compositor's own callback, one period later, always passes.
  CHECK(vsync_cap_wait_ns(kT0 + k60, kT0, k60) == 0);
  CHECK(vsync_cap_wait_ns(kT0 + 10 * k60, kT0, k60) == 0);
}

TEST_CASE("vsync cap: an unmeasured refresh means 60 Hz", "[vsync]") {
  CHECK(vsync_min_gap_ns(0) == vsync_min_gap_ns(k60));
  CHECK(vsync_cap_wait_ns(kT0, kT0, 0) == vsync_min_gap_ns(k60));
}

TEST_CASE("vsync cap: the gap follows the refresh rate", "[vsync]") {
  constexpr std::uint64_t k120 = 8'333'333ull;
  CHECK(vsync_cap_wait_ns(kT0 + 8'000'000, kT0, k120) == 0);
  CHECK(vsync_cap_wait_ns(kT0 + 8'000'000, kT0, k60) != 0);
}

TEST_CASE("vsync cap: a clock that went backwards does not stall", "[vsync]") {
  CHECK(vsync_cap_wait_ns(kT0 - 1, kT0, k60) == 0);
}

TEST_CASE("vsync cap: waiting the returned time is always enough", "[vsync]") {
  // The retry asks again after the wait; it must then be delivered, or the
  // loop would spin on retries.
  for (std::uint64_t refresh : {std::uint64_t{0}, k60, std::uint64_t{6'944'444},
                                std::uint64_t{33'333'333}}) {
    for (std::uint64_t early = 0; early < vsync_min_gap_ns(refresh);
         early += 250'000) {
      const std::uint64_t wait = vsync_cap_wait_ns(kT0 + early, kT0, refresh);
      REQUIRE(wait != 0);
      REQUIRE(vsync_cap_wait_ns(kT0 + early + wait, kT0, refresh) == 0);
    }
  }
}

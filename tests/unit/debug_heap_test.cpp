// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test. No GPL-3.0 sources.
//
// The sizes under android.os.Debug.getNativeHeap*Size, read both ways the C
// library gives them: mallinfo2, and the mallinfo that is all glibc has
// before 2.33.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "runtime/debug_heap_jni.h"

using acl::runtime::native_heap_sizes;
using acl::runtime::NativeHeapSizes;

namespace {

struct Freed {
  void operator()(void* p) const noexcept { std::free(p); }
};
using Block = std::unique_ptr<void, Freed>;

// Allocated and touched, so the allocator has really taken it.
Block held(std::size_t bytes) {
  Block b(std::malloc(bytes));
  if (b)
    std::memset(b.get(), 0x5a, bytes);
  return b;
}

void check(bool legacy) {
  constexpr std::size_t kSmall = 256 * 1024;       // from an arena
  constexpr std::size_t kLarge = 8 * 1024 * 1024;  // mapped by itself
  const NativeHeapSizes before = native_heap_sizes(legacy);
  CHECK(before.total > 0);
  CHECK(before.allocated > 0);
  CHECK(before.allocated <= before.total);
  CHECK(before.free <= before.total);

  const Block small = held(kSmall);
  REQUIRE(small);
  const NativeHeapSizes with_small = native_heap_sizes(legacy);
  CHECK(with_small.allocated >= before.allocated + kSmall);

  const Block large = held(kLarge);
  REQUIRE(large);
  const NativeHeapSizes with_large = native_heap_sizes(legacy);
  CHECK(with_large.allocated >= with_small.allocated + kLarge);
  // The total is not compared across readings: the allocator may hand an
  // arena's free top back to the system in between.
  CHECK(with_large.total >= kLarge);
  CHECK(with_large.allocated <= with_large.total);
}

}  // namespace

TEST_CASE("native heap sizes: an allocation shows", "[runtime][heap]") {
  check(false);
}

TEST_CASE("native heap sizes: through mallinfo", "[runtime][heap]") {
  check(true);
}

TEST_CASE("native heap sizes: both readings agree", "[runtime][heap]") {
  const NativeHeapSizes now = native_heap_sizes(false);
  const NativeHeapSizes legacy = native_heap_sizes(true);
  // Nothing is allocated between the two, and a test's heap is far below
  // the 2 GiB where mallinfo's counts wrap.
  CHECK(now.total == legacy.total);
  CHECK(now.allocated == legacy.allocated);
  CHECK(now.free == legacy.free);
}

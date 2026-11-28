// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test code; bionic's TLS slot numbers from AOSP
// bionic libc/platform/bionic/tls_defines.h (BSD-3-Clause). No GPL-3.0
// sources.
//
// The thread pointer of a thread of this process, which glibc made, is told
// from a block laid out as bionic lays out its own. The check read only
// x86-64's layout once: on AArch64 it took every thread for a bionic one.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>

#include "ndk/thread_pointer.h"

using acl::ndk::is_glibc_thread_pointer;

#if defined(__GLIBC__) && (defined(__x86_64__) || defined(__aarch64__))

namespace {
std::uintptr_t this_thread_pointer() {
  std::uintptr_t tp = 0;
#if defined(__x86_64__)
  asm volatile("mov %%fs:0, %0" : "=r"(tp));
#else
  asm volatile("mrs %0, tpidr_el0" : "=r"(tp));
#endif
  return tp;
}
}  // namespace

TEST_CASE("the main thread's thread pointer is glibc's", "[ndk][tls]") {
  CHECK(is_glibc_thread_pointer(this_thread_pointer()));
}

TEST_CASE("the thread pointer of a thread glibc made is glibc's",
          "[ndk][tls]") {
  // The second thread is given the stack, and with it the block, of the first.
  for (int round = 0; round < 2; ++round) {
    bool glibc = false;
    std::thread t(
        [&glibc] { glibc = is_glibc_thread_pointer(this_thread_pointer()); });
    t.join();
    INFO("thread " << round);
    CHECK(glibc);
  }
}

TEST_CASE("a block laid out as bionic's is not glibc's", "[ndk][tls]") {
  std::uintptr_t block[12] = {};
#if defined(__x86_64__)
  // TLS_SLOT_SELF is slot 0. Slot 2 is TLS_SLOT_APP, whatever the app put
  // there.
  const auto tp = reinterpret_cast<std::uintptr_t>(&block[0]);
  block[0] = tp;
  CHECK_FALSE(is_glibc_thread_pointer(tp));
  block[2] = 0x1234;
  CHECK_FALSE(is_glibc_thread_pointer(tp));
#else
  // Slot -1 is under the thread pointer. Slot 1 is TLS_SLOT_THREAD_ID, the
  // thread's pthread_internal_t.
  const auto tp = reinterpret_cast<std::uintptr_t>(&block[1]);
  block[2] = reinterpret_cast<std::uintptr_t>(&block[4]);
  CHECK_FALSE(is_glibc_thread_pointer(tp));
#endif
}

#endif  // glibc on x86-64 or AArch64

TEST_CASE("no thread pointer is not glibc's", "[ndk][tls]") {
  CHECK_FALSE(is_glibc_thread_pointer(0));
}

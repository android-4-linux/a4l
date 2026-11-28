// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. The thread-control-block layouts are the public
// ABI of each libc: glibc's tcbhead_t for x86-64 (tcb and self both hold the
// thread pointer) and for AArch64 (dtv, then a word glibc leaves unused);
// bionic's TLS slots from AOSP bionic libc/platform/bionic/tls_defines.h
// (BSD-3-Clause). No GPL-3.0 sources.
//
// Which libc a thread pointer belongs to, read off the thread pointer alone.
// The signal trampoline in bionic_runtime.cpp asks this of the thread a fault
// landed on, so it is two loads and nothing else: no call, no thread-local.
#ifndef ACL_NDK_THREAD_POINTER_H
#define ACL_NDK_THREAD_POINTER_H

#include <cstdint>

namespace acl::ndk {

// True if `tp` is a glibc thread pointer, false if it is bionic's. `tp` must be
// the thread pointer of a live thread, so the words read are mapped.
//
// x86-64: glibc's tcbhead_t holds the thread pointer itself at 0x00 (tcb) and
// at 0x10 (self). bionic's block does at 0x00 too (TLS_SLOT_SELF), but 0x10 is
// TLS_SLOT_APP, never the base.
//
// AArch64: neither libc keeps a self pointer. The word at 0x08 is glibc's
// tcbhead_t.private, which glibc never writes and so stays zero, and bionic's
// TLS_SLOT_THREAD_ID, the thread's pthread_internal_t, set before the thread
// first runs.
inline bool is_glibc_thread_pointer(std::uintptr_t tp) noexcept {
  if (tp == 0)
    return false;
  const auto* word = reinterpret_cast<const std::uintptr_t*>(tp);
#if defined(__x86_64__)
  return word[0] == tp && word[2] == tp;
#elif defined(__aarch64__)
  return word[1] == 0;
#else
  (void)word;
  return false;  // no bionic runtime on this architecture
#endif
}

}  // namespace acl::ndk

#endif  // ACL_NDK_THREAD_POINTER_H

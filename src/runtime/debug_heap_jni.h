// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. No GPL-3.0 sources.
#ifndef ACL_RUNTIME_DEBUG_HEAP_JNI_H
#define ACL_RUNTIME_DEBUG_HEAP_JNI_H

#include <jni.h>

#include <cstdint>

namespace acl::runtime {

struct NativeHeapSizes {
  std::uint64_t total = 0;      // held from the system
  std::uint64_t allocated = 0;  // in use by the process
  std::uint64_t free = 0;       // held and not in use
};

// The C library's account of its allocator. `legacy` reads it through
// mallinfo, which is all glibc has before 2.33 and counts in int: wrong past
// 2 GiB. Otherwise mallinfo2 where the library has it.
NativeHeapSizes native_heap_sizes(bool legacy = false) noexcept;

// Binds android.os.Debug.getNativeHeapSize, getNativeHeapAllocatedSize and
// getNativeHeapFreeSize.
void register_debug_heap_natives(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_DEBUG_HEAP_JNI_H

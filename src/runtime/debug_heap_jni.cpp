// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l; what each size means is as android.os.Debug
// documents it (AOSP, Apache-2.0). No GPL-3.0 sources.
//
// android.os.Debug's native heap sizes, from the C library's own account of
// its allocator. An app samples them on a timer for its memory reports; with
// no native side each sample is an UnsatisfiedLinkError on that thread.
#include "runtime/debug_heap_jni.h"

#include <malloc.h>

namespace acl::runtime {
namespace {

template <typename Info>
NativeHeapSizes sizes_from(const Info& mi) noexcept {
  const auto arena = static_cast<std::uint64_t>(mi.arena);
  // Blocks the allocator mapped one by one, outside its arenas.
  const auto mapped = static_cast<std::uint64_t>(mi.hblkhd);
  NativeHeapSizes s;
  s.total = arena + mapped;
  s.allocated = static_cast<std::uint64_t>(mi.uordblks) + mapped;
  s.free = static_cast<std::uint64_t>(mi.fordblks);
  return s;
}

}  // namespace

NativeHeapSizes native_heap_sizes([[maybe_unused]] bool legacy) noexcept {
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 33)
  if (!legacy)
    return sizes_from(mallinfo2());
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  return sizes_from(mallinfo());
#pragma GCC diagnostic pop
#else
  return {};
#endif
}

namespace {

jlong JNICALL native_heap_size(JNIEnv*, jclass) {
  return static_cast<jlong>(native_heap_sizes().total);
}

jlong JNICALL native_heap_allocated(JNIEnv*, jclass) {
  return static_cast<jlong>(native_heap_sizes().allocated);
}

jlong JNICALL native_heap_free(JNIEnv*, jclass) {
  return static_cast<jlong>(native_heap_sizes().free);
}

}  // namespace

void register_debug_heap_natives(JNIEnv* env) {
  jclass c = env->FindClass("android/os/Debug");
  if (c == nullptr) {
    env->ExceptionClear();
    return;
  }
  const JNINativeMethod methods[] = {
      {const_cast<char*>("getNativeHeapSize"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&native_heap_size)},
      {const_cast<char*>("getNativeHeapAllocatedSize"),
       const_cast<char*>("()J"),
       reinterpret_cast<void*>(&native_heap_allocated)},
      {const_cast<char*>("getNativeHeapFreeSize"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&native_heap_free)},
  };
  // One at a time: the host runtime may already have some of them.
  for (const JNINativeMethod& m : methods) {
    if (env->RegisterNatives(c, &m, 1) != JNI_OK)
      env->ExceptionClear();
  }
  env->DeleteLocalRef(c);
}

}  // namespace acl::runtime

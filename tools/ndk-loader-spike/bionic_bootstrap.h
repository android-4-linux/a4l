// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike. See bionic_bootstrap.cpp. No GPL-3.0 sources.
#pragma once

namespace acl::ndk::spike {

struct Config {
  const char* linker_path = nullptr;    // AOSP bionic linker64
  const char* stub_path = nullptr;      // minimal bionic PIE exe for the linker to "load"
  const char* bionic_libdir = nullptr;  // dir with bionic libc/libm/libdl/liblog/ld-android.so
  const char* so_path = nullptr;        // the bionic .so to dlopen (absolute)
  const char* symbol = nullptr;         // symbol to resolve + call
};

// In-process bootstrap the bionic linker, dlopen so_path, resolve + call `symbol` (as an
// int(JNIEnv*,jclass)). Returns the callee's int result, or a negative bootstrap error code.
int run(const Config& cfg);

}  // namespace acl::ndk::spike

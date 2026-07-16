// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl. Mirrors the other src/**/ *_probe mains. Exercises the in-process
// bionic loader (src/ndk/bionic_runtime) against a bionic .so. No GPL-3.0 sources.
//
// Usage: acl-ndk-probe <target.so> <symbol>
//   env: ACL_BIONIC_LINKER, ACL_BIONIC_SYSROOT, ACL_BIONIC_STUB (see docs/ndk-scope.md).
// Loads the bionic .so in-process on the glibc host, resolves <symbol>, and calls it as an
// int(JNIEnv*,jclass) (the tools/ndk-loader-spike repro's nativeAnswer ignores both args).
// Prints the returned int; exit 0 iff it returns 42 (the repro's value).

#include <cstdio>
#include <string>

#include "ndk/bionic_runtime.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <abs-path-to.so> <symbol>\n", argv[0]);
    return 2;
  }
  acl::ndk::BionicConfig cfg = acl::ndk::BionicConfig::from_env();
  std::string err;
  acl::ndk::BionicRuntime* rt = acl::ndk::BionicRuntime::instance(cfg, &err);
  if (!rt) {
    std::fprintf(stderr, "bootstrap failed: %s\n", err.c_str());
    return 1;
  }
  void* h = rt->load(argv[1], &err);
  if (!h) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  void* fn = rt->symbol(h, argv[2]);
  if (!fn) {
    std::fprintf(stderr, "symbol '%s' not found\n", argv[2]);
    return 1;
  }
  int result;
  {
    acl::ndk::BionicRuntime::RunScope scope(*rt);
    result = reinterpret_cast<int (*)(void*, void*)>(fn)(nullptr, nullptr);
  }
  std::printf("acl-ndk-probe: %s() = %d %s\n", argv[2], result, result == 42 ? "(PASS)" : "");
  return result == 42 ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike. See bionic_bootstrap.cpp / README.md. No GPL-3.0 sources.
//
// Harness for the in-process bionic-linker bootstrap: load a bionic .so and call nativeAnswer().
// Paths come from argv (the build script fills them). Success = "42".

#include <cstdio>
#include <cstdlib>

#include "bionic_bootstrap.h"

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr,
                 "usage: %s <linker64> <stub-exe> <bionic-libdir> <target.so> <symbol>\n", argv[0]);
    return 2;
  }
  acl::ndk::spike::Config cfg;
  cfg.linker_path = argv[1];
  cfg.stub_path = argv[2];
  cfg.bionic_libdir = argv[3];
  cfg.so_path = argv[4];
  cfg.symbol = argv[5];

  const int r = acl::ndk::spike::run(cfg);
  if (r < 0) {
    std::fprintf(stderr, "SPIKE FAILED: bootstrap error %d\n", r);
    return 1;
  }
  std::printf("SPIKE: nativeAnswer() = %d %s\n", r, r == 42 ? "(PASS)" : "(unexpected)");
  return r == 42 ? 0 : 1;
}

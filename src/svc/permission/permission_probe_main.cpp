// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l dev tool over svc/permission. No GPL-3.0 sources.
//
// acl-permission-probe <device> <app>: print what the desktop's permission
// store holds for the app and the device, as a4l reads it.
#include <cstdio>

#include "svc/permission/permission_store.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: acl-permission-probe <device> <app>\n");
    return 2;
  }
  const auto access = acl::permission::query_store(argv[1], argv[2]);
  std::printf("%s %s: %s\n", argv[1], argv[2], acl::permission::name(access));
  return access == acl::permission::Access::kYes ? 0 : 1;
}

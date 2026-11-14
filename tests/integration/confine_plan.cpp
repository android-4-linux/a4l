// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test helper (docs/adr/ADR-0009 C4). No GPL-3.0
// sources.
//
// Prints the confinement plan a4l would use for an APK, so that a test can
// run a probe of its own choosing inside the same sandbox instead of a4l:
//
//   confine_plan <apk> <package-dir> [--net]
//
// One line per bubblewrap argument, a line "--ENV--", then one line per
// environment variable. Exit 77 if this host cannot confine (no bubblewrap,
// no user namespaces), which Meson reads as a skip.

#include <cstdio>
#include <cstring>
#include <string>

#include "confine/confine.h"

extern char** environ;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: confine_plan <apk> <package-dir> [--net]\n");
    return 2;
  }
  acl::confine::Inputs in;
  in.apk = argv[1];
  in.package_dir = argv[2];
  in.share_net = argc > 3 && std::strcmp(argv[3], "--net") == 0;
  acl::confine::Launch launch;
  std::string error;
  if (!acl::confine::plan_launch(in, environ, launch, error)) {
    std::fprintf(stderr, "confine_plan: %s\n", error.c_str());
    const bool refused = error.rfind("refusing", 0) == 0;
    return refused ? 3 : 77;
  }
  for (const std::string& a : acl::confine::bwrap_argv(launch.plan))
    std::printf("%s\n", a.c_str());
  std::printf("--ENV--\n");
  for (const std::string& e : launch.env)
    std::printf("%s\n", e.c_str());
  return 0;
}

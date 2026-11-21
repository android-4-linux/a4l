// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test. No GPL-3.0 sources.
//
// media::ff_open takes an FFmpeg library only at the major acl was built
// against. Run against the host's own libavutil: the test asks for its real
// major and for the next one, which has no runtime soname, so ff_open falls
// back to the unversioned -dev symlink and must refuse what it finds there.
// SKIPs on a host without libavutil's -dev symlink.

#include <catch2/catch_test_macros.hpp>

#include <dlfcn.h>

#include "media/ff_open.h"

TEST_CASE("ff_open refuses an FFmpeg library of another major", "[media]") {
  void* any = ::dlopen("libavutil.so", RTLD_NOW | RTLD_LOCAL);
  if (any == nullptr)
    SKIP("no libavutil.so on this host");
  using VersionFn = unsigned (*)();
  const auto version =
      reinterpret_cast<VersionFn>(::dlsym(any, "avutil_version"));
  REQUIRE(version != nullptr);
  const int major = static_cast<int>(version() >> 16);

  void* same =
      acl::media::ff_open("test", "libavutil.so", major, "avutil_version");
  CHECK(same != nullptr);
  CHECK(acl::media::ff_open("test", "libavutil.so", major + 1,
                            "avutil_version") == nullptr);
  CHECK(acl::media::ff_open("test", "libavutil.so", major,
                            "no_such_version_fn") == nullptr);
  if (same != nullptr)
    ::dlclose(same);
  ::dlclose(any);
}

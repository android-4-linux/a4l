// SPDX-License-Identifier: Apache-2.0
// Provenance: FFmpeg public API (libav*, LGPL, dynamic link only): each
// library's <name>_version() returns its version as major << 16 | minor << 8
// | micro. dlopen(3) and dlsym(3). No GPL-3.0 sources.
//
// Opens one FFmpeg library at run time, RTLD_LOCAL, and only if its major
// version is the one acl was built against. Structs and enums change between
// majors, so a library of another major loads and binds, then crashes: the
// video probe built for FFmpeg 4 did on FFmpeg 7. The runtime soname
// (libavcodec.so.58) carries the major; the unversioned -dev symlink, tried
// when that name is missing, may not.

#ifndef ACL_MEDIA_FF_OPEN_H
#define ACL_MEDIA_FF_OPEN_H

#include <dlfcn.h>

#include <cstdio>
#include <string_view>

#include "log/log.h"

namespace acl::media {

// `base` is the unversioned name ("libavcodec.so"), `major` the header's
// LIB*_VERSION_MAJOR and `version_fn` the library's own version function
// ("avcodec_version"). Returns nullptr, after a warning under `tag`, for a
// library of another major or one with no version function.
inline void* ff_open(std::string_view tag,
                     const char* base,
                     int major,
                     const char* version_fn) {
  char name[64];
  std::snprintf(name, sizeof(name), "%s.%d", base, major);
  void* h = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
  if (h == nullptr)
    h = ::dlopen(base, RTLD_NOW | RTLD_LOCAL);
  if (h == nullptr)
    return nullptr;
  using VersionFn = unsigned (*)();
  const auto version = reinterpret_cast<VersionFn>(::dlsym(h, version_fn));
  if (version == nullptr) {
    log::w(tag, "{} has no {}; not used", base, version_fn);
    ::dlclose(h);
    return nullptr;
  }
  if (const int found = static_cast<int>(version() >> 16); found != major) {
    log::w(tag, "{} is major {}, built for {}; not used", base, found, major);
    ::dlclose(h);
    return nullptr;
  }
  return h;
}

}  // namespace acl::media

#endif  // ACL_MEDIA_FF_OPEN_H

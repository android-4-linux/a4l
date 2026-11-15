// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (looper-driven present). No GPL-3.0 sources.
//
// The vsync rate cap of the looper-driven present path: at most one vsync
// per refresh period. A vsync that comes early is held, not dropped. The
// caller is told how long to wait and must ask again after that, because a
// compositor that has already sent its frame callback sends nothing more to
// wake the loop.
#ifndef ACL_RUNTIME_VSYNC_CAP_H
#define ACL_RUNTIME_VSYNC_CAP_H

#include <cstdint>

namespace acl::runtime {

// Shortest gap allowed between two vsyncs: 7/8 of the refresh period, so a
// compositor's own callbacks, which jitter around one period, always pass.
// A refresh of 0 (not yet measured) means 60 Hz.
constexpr std::uint64_t vsync_min_gap_ns(std::uint64_t refresh_ns) noexcept {
  const std::uint64_t refresh = refresh_ns > 0 ? refresh_ns : 16'666'667ull;
  return refresh - refresh / 8;
}

// Nanoseconds until a vsync may be delivered: 0 means now. `last_ns` is the
// time of the previous delivery, 0 for none. A clock that went backwards
// delivers at once.
constexpr std::uint64_t vsync_cap_wait_ns(std::uint64_t now_ns,
                                          std::uint64_t last_ns,
                                          std::uint64_t refresh_ns) noexcept {
  if (last_ns == 0 || now_ns < last_ns)
    return 0;
  const std::uint64_t gap = vsync_min_gap_ns(refresh_ns);
  const std::uint64_t elapsed = now_ns - last_ns;
  return elapsed >= gap ? 0 : gap - elapsed;
}

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_VSYNC_CAP_H

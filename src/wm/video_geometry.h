// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (video plane; wp_viewporter protocol
// documentation for the constraints). No GPL-3.0 sources.
//
// Where a decoded video frame goes on screen. The app lays its video View
// out at some rectangle in its surface, possibly larger than the window and
// partly off it (a zoom that crops). The plane is a subsurface, which the
// compositor does not clip to the parent, so the rectangle is cut to the
// window here and the frame is cropped to match.
//
// The result goes to wp_viewport, which answers a source rectangle that is
// empty or reaches outside the buffer with a protocol error, and that ends
// the connection. Under --split the rectangle comes from the app process, so
// every result this returns is one the compositor accepts.
#ifndef ACL_WM_VIDEO_GEOMETRY_H
#define ACL_WM_VIDEO_GEOMETRY_H

#include <cmath>
#include <cstdint>

namespace acl::wm {

struct VideoPlacement {
  bool visible = false;  // false: nothing of the video is in the window
  int x = 0, y = 0;      // top-left in the window
  int w = 0, h = 0;      // size on screen
  // The part of the buffer shown, in buffer pixels, on the 1/256 grid the
  // protocol carries.
  double sx = 0, sy = 0, sw = 0, sh = 0;
};

// `x,y,w,h`: the video View in the app surface. `win_w,win_h`: the window.
// `buf_w,buf_h`: the decoded frame.
inline VideoPlacement place_video(int x,
                                  int y,
                                  int w,
                                  int h,
                                  int win_w,
                                  int win_h,
                                  std::uint32_t buf_w,
                                  std::uint32_t buf_h) noexcept {
  VideoPlacement p;
  if (w <= 0 || h <= 0 || win_w <= 0 || win_h <= 0 || buf_w == 0 || buf_h == 0)
    return p;
  // 64-bit: x + w must not wrap whatever the caller passed.
  const std::int64_t left = x > 0 ? x : 0;
  const std::int64_t top = y > 0 ? y : 0;
  const std::int64_t right =
      std::int64_t{x} + w < win_w ? std::int64_t{x} + w : win_w;
  const std::int64_t bottom =
      std::int64_t{y} + h < win_h ? std::int64_t{y} + h : win_h;
  if (right <= left || bottom <= top)
    return p;
  const auto grid = [](double v) { return std::floor(v * 256.0) / 256.0; };
  const auto span = [&](std::int64_t from, std::int64_t len,
                        std::int64_t origin, std::int64_t extent,
                        std::uint32_t buf, double& s0, double& s1) {
    const double b = static_cast<double>(buf);
    double start = grid(static_cast<double>(from - origin) / extent * b);
    if (start < 0)
      start = 0;
    if (start > b)
      start = b;
    double size = grid(static_cast<double>(len) / extent * b);
    if (size > b - start)
      size = b - start;
    s0 = start;
    s1 = size;
  };
  span(left, right - left, x, w, buf_w, p.sx, p.sw);
  span(top, bottom - top, y, h, buf_h, p.sy, p.sh);
  // Less than one buffer pixel across is nothing to show, and at zero it is
  // a protocol error.
  if (p.sw < 1.0 || p.sh < 1.0)
    return p;
  p.x = static_cast<int>(left);
  p.y = static_cast<int>(top);
  p.w = static_cast<int>(right - left);
  p.h = static_cast<int>(bottom - top);
  p.visible = true;
  return p;
}

}  // namespace acl::wm

#endif  // ACL_WM_VIDEO_GEOMETRY_H

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l code; the EGL 1.4 specification's attribute lists
// (EGL_SURFACE_TYPE and its bits). No GPL-3.0 sources.
#ifndef ACL_GFX_EGL_ATTRIBS_H
#define ACL_GFX_EGL_ATTRIBS_H

#include <cstddef>
#include <vector>

namespace acl::gfx {

// EGL's values, so that this header needs no EGL headers. EGLint is an int.
inline constexpr int kEglNone = 0x3038;
inline constexpr int kEglSurfaceType = 0x3033;
inline constexpr int kEglPbufferBit = 0x0001;
inline constexpr int kEglWindowBit = 0x0004;

// A copy of the EGL_NONE-terminated config attribute list `attribs` in which
// an EGL_SURFACE_TYPE that asks for `from_bit` asks for `to_bit` instead. For
// a display that has configs for one kind of surface only: Mesa's gbm
// platform has window configs and no pbuffer ones, its surfaceless platform
// the reverse. Empty when the list is null, does not ask for `from_bit`, or
// is longer than a config attribute list can be.
inline std::vector<int> egl_surface_type_swapped(const int* attribs,
                                                 int from_bit,
                                                 int to_bit) {
  constexpr std::size_t kMaxPairs = 64;
  std::vector<int> out;
  if (attribs == nullptr)
    return out;
  bool asked = false;
  for (const int* p = attribs; *p != kEglNone; p += 2) {
    if (out.size() >= 2 * kMaxPairs)
      return {};  // not an attribute list
    int value = p[1];
    if (p[0] == kEglSurfaceType && (value & from_bit) != 0) {
      value = (value & ~from_bit) | to_bit;
      asked = true;
    }
    out.push_back(p[0]);
    out.push_back(value);
  }
  if (!asked)
    return {};
  out.push_back(kEglNone);
  return out;
}

}  // namespace acl::gfx

#endif  // ACL_GFX_EGL_ATTRIBS_H

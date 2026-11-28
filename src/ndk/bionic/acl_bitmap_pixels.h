// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l; the pixel layouts are those of Android's public
// android.graphics.Color and <android/bitmap.h> documentation. No GPL-3.0
// sources.
//
// The two layouts a Bitmap's pixels have on the way through a4l's
// libjnigraphics.so, and the conversion between them.
//
// Bitmap.getPixels and setPixels deal in Color ints: 0xAARRGGBB, not
// premultiplied. A caller of AndroidBitmap_lockPixels reads and writes
// ANDROID_BITMAP_FORMAT_RGBA_8888: the bytes R, G, B, A in that order,
// premultiplied unless the Bitmap says it is not.
//
// Plain C: compiled for bionic (the shim) and for the host (its unit test).
#ifndef ACL_NDK_BIONIC_ACL_BITMAP_PIXELS_H_
#define ACL_NDK_BIONIC_ACL_BITMAP_PIXELS_H_

#include <stddef.h>
#include <stdint.h>

// `n` Color ints at `p`, in place, to RGBA_8888 bytes. `premul`: the colors are
// multiplied by their alpha, rounded to nearest.
static inline void acl_colors_to_rgba(uint8_t* p, size_t n, int premul) {
  for (size_t i = 0; i < n; ++i, p += 4) {
    uint32_t c;
    __builtin_memcpy(&c, p, 4);
    uint32_t r = (c >> 16) & 0xffu, g = (c >> 8) & 0xffu, b = c & 0xffu;
    const uint32_t a = c >> 24;
    if (premul && a != 0xffu) {
      r = (r * a + 127u) / 255u;
      g = (g * a + 127u) / 255u;
      b = (b * a + 127u) / 255u;
    }
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
    p[3] = (uint8_t)a;
  }
}

// `n` RGBA_8888 pixels at `p`, in place, to Color ints. `premul`: the colors
// are divided by their alpha, rounded to nearest. One that is brighter than
// its alpha allows is held at full, and one with no alpha is black.
static inline void acl_rgba_to_colors(uint8_t* p, size_t n, int premul) {
  for (size_t i = 0; i < n; ++i, p += 4) {
    uint32_t r = p[0], g = p[1], b = p[2];
    const uint32_t a = p[3];
    if (premul && a != 0xffu) {
      if (a == 0u) {
        r = g = b = 0u;
      } else {
        r = (r * 255u + a / 2u) / a;
        g = (g * 255u + a / 2u) / a;
        b = (b * 255u + a / 2u) / a;
        if (r > 255u)
          r = 255u;
        if (g > 255u)
          g = 255u;
        if (b > 255u)
          b = 255u;
      }
    }
    const uint32_t c = (a << 24) | (r << 16) | (g << 8) | b;
    __builtin_memcpy(p, &c, 4);
  }
}

#endif  // ACL_NDK_BIONIC_ACL_BITMAP_PIXELS_H_

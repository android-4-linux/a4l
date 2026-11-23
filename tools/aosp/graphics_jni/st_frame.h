// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation; Linux dma-buf / DRM fourcc plane
// layout. No GPL-3.0 sources.
//
// The frames a4l sends a SurfaceTexture, as graphics_jni takes them; shared
// by graphics_jni.cpp and gpu_dmabuf.cpp. a4l defines both too
// (media_codec_jni.cpp, gfx/surface_texture_gl.h); the definitions must
// match.
#ifndef ACL_GRAPHICS_JNI_ST_FRAME_H
#define ACL_GRAPHICS_JNI_ST_FRAME_H

#include <cstdint>

// A decoder's frame converted to XBGR8888 (acl_graphics_surfacetexture_set_
// frame): one dma-buf plane, already cropped to the picture.
struct AclStFrame {
  int fd;
  uint32_t width, height, stride, offset, drm_format;
  uint64_t modifier;
};

// A decoder's frame as it is (acl_graphics_surfacetexture_set_yuv_frame):
// every plane, uncropped, with the transform an app's texture coordinates
// need. fd[i] is plane i's dma-buf; planes in one buffer may name it by
// different descriptors.
struct AclStYuvFrame {
  uint32_t width, height;   // the picture
  uint32_t crop_x, crop_y;  // where it starts in the buffer
  uint32_t drm_format;
  uint64_t modifier;
  uint32_t plane_count;
  int fd[4];
  uint32_t offset[4], stride[4];
  uint8_t yuv_matrix;  // H.273 matrix coefficients: 1, 6 or 9
  uint8_t yuv_full_range;
  float transform[16];  // SurfaceTexture.getTransformMatrix, column-major
};

#endif  // ACL_GRAPHICS_JNI_ST_FRAME_H

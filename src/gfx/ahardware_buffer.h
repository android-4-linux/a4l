// SPDX-License-Identifier: Apache-2.0
// Provenance: Android NDK android/hardware_buffer.h (the opaque AHardwareBuffer
// type + AHardwareBuffer_Desc layout + AHARDWAREBUFFER_FORMAT_*/USAGE_* — the
// ABI contract only), EGL_ANDROID_get_native_client_buffer + EGL_KHR_image_base
// + EGL_EXT_image_dma_buf_import (Khronos), Mesa EGL. The M2 slice of the
// Flutter external-texture bridge
// ([[flutter-texture-bridge]]): the engine imports a Java HardwareBuffer as an
// EGLImage via eglGetNativeClientBufferANDROID +
// eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID); host Mesa knows neither, so acl
// represents an AHardwareBuffer as a dma-buf and translates that import into
// eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT). No GPL-3.0 sources.

#ifndef ACL_GFX_AHARDWARE_BUFFER_H
#define ACL_GFX_AHARDWARE_BUFFER_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <cstdint>
#include <memory>

#include "gfx/hardware_buffer.h"

// Mirrors the NDK AHardwareBuffer_Desc field-for-field so the JNI layer (M3)
// and any app NDK caller see the same ABI. `format` is an
// AHARDWAREBUFFER_FORMAT_*, `usage` a bitmask of AHARDWAREBUFFER_USAGE_*.
struct AHardwareBuffer_Desc {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t layers;
  std::uint32_t format;
  std::uint64_t usage;
  std::uint32_t stride;  // in pixels
  std::uint32_t rfu0;
  std::uint64_t rfu1;
};

// acl's concrete definition of the NDK's opaque `AHardwareBuffer`. A refcounted
// dma-buf: it owns the backing gfx::HardwareBuffer (when acl allocated it) and
// carries the exported frame so the EGL translation and the C ABI read it with
// no round-trip. The engine only ever holds it by pointer.
struct AHardwareBuffer {
  std::shared_ptr<acl::gfx::HardwareBuffer>
      buffer;  // the gbm_bo owner (dma-buf lives here)
  AHardwareBuffer_Desc desc{};
};

namespace acl::gfx {

// Wrap an allocated HardwareBuffer as an AHardwareBuffer (shares ownership).
// The returned handle is released with ahb_release(); refcount starts at 1.
AHardwareBuffer* ahb_create(std::shared_ptr<HardwareBuffer> hb);

// Allocate directly (AHardwareBuffer_allocate equivalent):
// width/height/format(HbFormat value)/usage. Returns nullptr on failure (no
// render node / gbm). refcount 1.
AHardwareBuffer* ahb_allocate(const AHardwareBuffer_Desc& desc);

// Import an external dma-buf (a video decoder's DRM_PRIME frame) as an
// AHardwareBuffer, ZERO-COPY (dup fd + record layout, no pixel copy — see
// HardwareBuffer::import). refcount 1; nullptr on failure. The engine imports
// frame() directly, so a decoded frame reaches the compositor with no copy.
AHardwareBuffer* ahb_import(const DmabufFrame& frame);

void ahb_acquire(AHardwareBuffer* ahb);
void ahb_release(AHardwareBuffer* ahb);
void ahb_describe(const AHardwareBuffer* ahb, AHardwareBuffer_Desc* out);

// Fill the whole buffer with a solid color (0xAARRGGBB) via a CPU lock — the
// acl producer's frame source for the external-texture proof. Byte order
// follows the buffer's DRM fourcc (RGBA/ABGR8888).
void ahb_fill_color(AHardwareBuffer* ahb, std::uint32_t argb);

// CPU-map the buffer for a producer to write into (AHardwareBuffer_lock /
// ANativeWindow_lock backing): returns the mapped base pointer and sets
// *out_stride_bytes to the row pitch in BYTES; nullptr on failure. ahb_unlock()
// must follow. The bytes are laid out per the buffer's DRM fourcc (ABGR8888).
void* ahb_lock(AHardwareBuffer* ahb, std::uint32_t* out_stride_bytes);
void ahb_unlock(AHardwareBuffer* ahb);

// The pointer signature of host Mesa's eglCreateImageKHR (dpy, ctx, target,
// buffer, attrib_list).
using EglCreateImageFn = void* (*)(void* dpy,
                                   void* ctx,
                                   unsigned target,
                                   void* buffer,
                                   const std::int32_t* attrib_list);

// The core M2 translation, kept pure for unit testing: build
// EGL_LINUX_DMA_BUF_EXT attribs from the AHardwareBuffer's dma-buf and call
// `real` (host Mesa's eglCreateImageKHR) with EGL_NO_CONTEXT and
// EGL_LINUX_DMA_BUF_EXT. `in_attribs` (the engine's original list, may be null)
// is scanned only to carry EGL_IMAGE_PRESERVED_KHR through. Returns the
// EGLImage or EGL_NO_IMAGE_KHR(nullptr).
void* egl_image_from_ahb(void* dpy,
                         const AHardwareBuffer* ahb,
                         const std::int32_t* in_attribs,
                         EglCreateImageFn real);

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
#endif  // ACL_GFX_AHARDWARE_BUFFER_H

// SPDX-License-Identifier: Apache-2.0
// Provenance: EGL_EXT_image_dma_buf_import (EGL_LINUX_DMA_BUF_EXT + EGL_DMA_BUF_PLANE0_* + the
// modifier attribs from EGL_EXT_image_dma_buf_import_modifiers), EGL_KHR_image_base
// (EGL_IMAGE_PRESERVED_KHR), Khronos EGL headers, NDK android/hardware_buffer.h contract. No GPL-3.0 sources.

#include "gfx/ahardware_buffer.h"

#if ACL_HAVE_GL

#include <atomic>
#include <cstdint>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>

#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "gfx.ahb";

// Refcount kept in a side map so `struct AHardwareBuffer` stays a clean ABI POD-ish handle (the NDK
// treats it as opaque). Simpler: hang an atomic count next to the handle via a small wrapper.
struct AhbBox {
  AHardwareBuffer pub;
  std::atomic<int> refs{1};
};
AhbBox* box_of(AHardwareBuffer* p) { return reinterpret_cast<AhbBox*>(p); }
}  // namespace

AHardwareBuffer* ahb_create(std::shared_ptr<HardwareBuffer> hb) {
  if (!hb) return nullptr;
  auto* box = new AhbBox();
  box->pub.buffer = std::move(hb);
  const auto& f = box->pub.buffer->frame();
  const auto& d = box->pub.buffer->desc();
  box->pub.desc = AHardwareBuffer_Desc{d.width, d.height, 1, static_cast<std::uint32_t>(d.format),
                                       d.usage, f.planes[0].stride / 4u, 0, 0};
  return &box->pub;  // pub is the first member — &pub == box, so box_of() round-trips
}

AHardwareBuffer* ahb_allocate(const AHardwareBuffer_Desc& desc) {
  std::string err;
  auto hb = HardwareBuffer::allocate(desc.width, desc.height, static_cast<HbFormat>(desc.format),
                                     desc.usage, &err);
  if (!hb) { log::w(kTag, "ahb_allocate {}x{} failed: {}", desc.width, desc.height, err); return nullptr; }
  return ahb_create(std::move(hb));
}

void ahb_acquire(AHardwareBuffer* ahb) {
  if (ahb == nullptr) return;
  const int n = box_of(ahb)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (getenv("ACL_AHB_DIAG") != nullptr) log::i(kTag, "acquire {} -> refs={}", (void*)ahb, n);
}

void ahb_release(AHardwareBuffer* ahb) {
  if (ahb == nullptr) return;
  const int prev = box_of(ahb)->refs.fetch_sub(1, std::memory_order_acq_rel);
  if (getenv("ACL_AHB_DIAG") != nullptr) log::i(kTag, "release {} -> refs={}", (void*)ahb, prev - 1);
  if (prev == 1) delete box_of(ahb);
}

void ahb_describe(const AHardwareBuffer* ahb, AHardwareBuffer_Desc* out) {
  if (ahb != nullptr && out != nullptr) *out = ahb->desc;
}

void ahb_fill_color(AHardwareBuffer* ahb, std::uint32_t argb) {
  if (ahb == nullptr || !ahb->buffer) return;
  std::string err;
  std::uint32_t stride = 0;
  auto* base = static_cast<std::uint8_t*>(ahb->buffer->lock(&stride, &err));
  if (base == nullptr) { log::w(kTag, "ahb_fill_color: lock failed: {}", err); return; }
  // The buffer fourcc is ABGR8888 (byte order R,G,B,A); lay the argb out that way.
  const std::uint8_t px[4] = {static_cast<std::uint8_t>(argb >> 16),  // R
                              static_cast<std::uint8_t>(argb >> 8),   // G
                              static_cast<std::uint8_t>(argb),        // B
                              static_cast<std::uint8_t>(argb >> 24)}; // A
  const std::uint32_t w = ahb->buffer->desc().width, h = ahb->buffer->desc().height;
  for (std::uint32_t y = 0; y < h; ++y) {
    std::uint8_t* row = base + static_cast<std::size_t>(y) * stride;
    for (std::uint32_t x = 0; x < w; ++x) { row[0] = px[0]; row[1] = px[1]; row[2] = px[2]; row[3] = px[3]; row += 4; }
  }
  ahb->buffer->unlock();
}

void* egl_image_from_ahb(void* dpy, const AHardwareBuffer* ahb, const std::int32_t* in_attribs,
                         EglCreateImageFn real) {
  if (ahb == nullptr || real == nullptr || !ahb->buffer) return nullptr;
  const DmabufFrame& f = ahb->buffer->frame();

  // Carry EGL_IMAGE_PRESERVED_KHR through if the engine asked for it.
  EGLint preserved = EGL_FALSE;
  for (const std::int32_t* a = in_attribs; a != nullptr && *a != EGL_NONE; a += 2)
    if (a[0] == EGL_IMAGE_PRESERVED_KHR) preserved = a[1];

  std::vector<EGLint> attrs = {
      EGL_WIDTH, static_cast<EGLint>(f.width),
      EGL_HEIGHT, static_cast<EGLint>(f.height),
      EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(f.drm_format),
      EGL_DMA_BUF_PLANE0_FD_EXT, f.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(f.planes[0].offset),
      EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(f.planes[0].stride),
  };
  // Pass the modifier only when it is a real, non-linear tiling (needs the modifiers extension).
  if (f.modifier != DRM_FORMAT_MOD_INVALID && f.modifier != DRM_FORMAT_MOD_LINEAR) {
    attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
    attrs.push_back(static_cast<EGLint>(f.modifier & 0xffffffffu));
    attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
    attrs.push_back(static_cast<EGLint>(f.modifier >> 32));
  }
  attrs.push_back(EGL_IMAGE_PRESERVED_KHR);
  attrs.push_back(preserved);
  attrs.push_back(EGL_NONE);

  void* img = real(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
  if (img == nullptr && getenv("ACL_GL_DIAG") != nullptr)
    log::w(kTag, "egl_image_from_ahb: dma-buf import failed (fourcc={:#x} fd={})", f.drm_format, f.fd);
  return img;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

// SPDX-License-Identifier: Apache-2.0
// Provenance: Android NDK public headers (android/hardware_buffer.h — AHardwareBuffer_Desc and the
// AHARDWAREBUFFER_FORMAT_*/USAGE_* constants, the contract only), gbm public API (gbm.h), libdrm
// drm_fourcc.h (DRM_FORMAT_*). The zero-copy interchange under Flutter's external-texture path
// ([[flutter-texture-bridge]]): a graphics buffer an app/producer fills and the engine imports as
// an EGLImage. Backs android.hardware.HardwareBuffer + AHardwareBuffer_* and, later, the
// android.media.Image the engine's ImageExternalTexture pulls. No GPL-3.0 sources.
//
// A HardwareBuffer is a single gbm_bo exported as a Linux dma-buf. It reuses gfx::DmabufFrame (the
// same plane/fd interchange the wm/ presenter and every GPU producer share), so the engine's
// eglCreateImageKHR import and acl's own present path consume it uniformly. Standalone from
// GlDevice: it owns a gbm_device on a DRM render node so a buffer can be allocated with no GL
// context (a producer thread just needs a buffer to fill).

#ifndef ACL_GFX_HARDWARE_BUFFER_H
#define ACL_GFX_HARDWARE_BUFFER_H

#include "acl_config.h"

#if ACL_HAVE_GL

#include <cstdint>
#include <memory>
#include <string>

#include "gfx/dmabuf_frame.h"

namespace acl::gfx {

// The subset of AHARDWAREBUFFER_FORMAT_* acl maps to DRM fourccs. Values match the NDK enum so the
// AHardwareBuffer C ABI (a later milestone) passes them straight through.
enum class HbFormat : std::uint32_t {
  R8G8B8A8_UNORM = 1,
  R8G8B8X8_UNORM = 2,
  R8G8B8_UNORM = 3,
  R5G6B5_UNORM = 4,
};

// Mirrors AHardwareBuffer_Desc (the fields acl uses). `usage` is advisory here — the gbm_bo is
// always allocated CPU-mappable + GPU-sampleable so both lock() and the engine's import work.
struct HbDesc {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  HbFormat format = HbFormat::R8G8B8A8_UNORM;
  std::uint64_t usage = 0;  // AHARDWAREBUFFER_USAGE_* bits, retained for describe()
};

// One dma-buf-backed graphics buffer. Move-only; owns the gbm_bo and the exported fd.
class HardwareBuffer {
 public:
  // Allocate a WxH buffer of `format`. Returns nullptr + *error when the host lacks a usable render
  // node / gbm (callers SKIP — they don't hard-fail). `usage` is stored for describe(); it does not
  // change the allocation (always linear + CPU-mappable + sampleable, the common denominator).
  static std::unique_ptr<HardwareBuffer> allocate(std::uint32_t width, std::uint32_t height,
                                                   HbFormat format, std::uint64_t usage,
                                                   std::string* error);

  ~HardwareBuffer();
  HardwareBuffer(const HardwareBuffer&) = delete;
  HardwareBuffer& operator=(const HardwareBuffer&) = delete;

  const HbDesc& desc() const { return desc_; }

  // The dma-buf export (fd + plane layout + DRM fourcc/modifier) for the engine's EGLImage import
  // and acl's presenter. The fd is owned by this HardwareBuffer and valid for its lifetime.
  const DmabufFrame& frame() const { return frame_; }

  // Map the buffer for CPU access (test-pattern fill / software producer). Returns the base pointer
  // and sets *stride to the row pitch in bytes; nullptr + *error on failure. unlock() must follow.
  void* lock(std::uint32_t* stride, std::string* error);
  void unlock();

 private:
  HardwareBuffer() = default;

  struct Gbm;                       // opaque: the shared gbm_device + render-node fd
  static std::shared_ptr<Gbm> shared_gbm(std::string* error);  // one device, reused per buffer
  std::shared_ptr<Gbm> gbm_;        // shared so all buffers reuse one device
  void* bo_ = nullptr;              // gbm_bo*
  void* map_data_ = nullptr;        // gbm_bo_map cookie (non-null while locked)
  HbDesc desc_;
  DmabufFrame frame_;
};

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
#endif  // ACL_GFX_HARDWARE_BUFFER_H

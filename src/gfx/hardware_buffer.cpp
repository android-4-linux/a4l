// SPDX-License-Identifier: Apache-2.0
// Provenance: gbm public API (gbm_create_device, gbm_bo_create, gbm_bo_map, gbm_bo_get_* — same
// calls gl_egl.cpp's gbm backend uses to export a front buffer), libdrm drm_fourcc.h (DRM_FORMAT_*),
// Android NDK android/hardware_buffer.h (the AHARDWAREBUFFER_FORMAT_* contract). No GPL-3.0 sources.

#include "gfx/hardware_buffer.h"

#if ACL_HAVE_GL

#include <fcntl.h>
#include <sys/mman.h>  // MAP_FAILED
#include <unistd.h>

#include <cstdlib>
#include <mutex>

#include <gbm.h>
#include <drm_fourcc.h>

#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "gfx.hwbuf";

std::uint32_t drm_fourcc_for(HbFormat f) {
  switch (f) {
    case HbFormat::R8G8B8A8_UNORM: return DRM_FORMAT_ABGR8888;
    case HbFormat::R8G8B8X8_UNORM: return DRM_FORMAT_XBGR8888;
    case HbFormat::R8G8B8_UNORM:   return DRM_FORMAT_BGR888;
    case HbFormat::R5G6B5_UNORM:   return DRM_FORMAT_RGB565;
  }
  return DRM_FORMAT_ABGR8888;
}
}  // namespace

// The shared gbm device: one DRM render node open for the whole process, reused by every buffer.
struct HardwareBuffer::Gbm {
  int fd = -1;
  gbm_device* dev = nullptr;
  ~Gbm() {
    if (dev != nullptr) gbm_device_destroy(dev);
    if (fd >= 0) ::close(fd);
  }
};

std::shared_ptr<HardwareBuffer::Gbm> HardwareBuffer::shared_gbm(std::string* error) {
  static std::mutex mu;
  static std::weak_ptr<HardwareBuffer::Gbm> cached;
  std::lock_guard<std::mutex> lock(mu);
  if (auto live = cached.lock()) return live;

  const char* node = std::getenv("ACL_DRM_RENDER_NODE");
  if (node == nullptr || *node == '\0') node = "/dev/dri/renderD128";
  auto g = std::make_shared<HardwareBuffer::Gbm>();
  g->fd = ::open(node, O_RDWR | O_CLOEXEC);
  if (g->fd < 0) { if (error) *error = std::string("open ") + node + " failed"; return nullptr; }
  g->dev = gbm_create_device(g->fd);
  if (g->dev == nullptr) { if (error) *error = std::string("gbm_create_device failed on ") + node; return nullptr; }
  cached = g;
  return g;
}

std::unique_ptr<HardwareBuffer> HardwareBuffer::allocate(std::uint32_t width, std::uint32_t height,
                                                         HbFormat format, std::uint64_t usage,
                                                         std::string* error) {
  if (width == 0 || height == 0) { if (error) *error = "zero-sized buffer"; return nullptr; }
  auto gbm = shared_gbm(error);
  if (!gbm) return nullptr;

  const std::uint32_t fourcc = drm_fourcc_for(format);
  // LINEAR so lock() can CPU-map it; RENDERING so the imported EGLImage is sampleable. This is the
  // common denominator both a software producer and the engine's texture import need.
  gbm_bo* bo = gbm_bo_create(gbm->dev, width, height, fourcc,
                             GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (bo == nullptr) { if (error) *error = "gbm_bo_create failed"; return nullptr; }

  const int fd = gbm_bo_get_fd(bo);  // a fresh dup we own; closed at destruction
  if (fd < 0) { gbm_bo_destroy(bo); if (error) *error = "gbm_bo_get_fd failed"; return nullptr; }

  auto hb = std::unique_ptr<HardwareBuffer>(new HardwareBuffer());
  hb->gbm_ = std::move(gbm);
  hb->bo_ = bo;
  hb->desc_ = HbDesc{width, height, format, usage};

  DmabufFrame& f = hb->frame_;
  f = DmabufFrame{};
  f.fd = fd;
  f.width = width;
  f.height = height;
  f.drm_format = fourcc;
  f.modifier = gbm_bo_get_modifier(bo);
  const int planes = gbm_bo_get_plane_count(bo);
  f.plane_count = planes > 0 ? static_cast<std::uint32_t>(planes) : 1u;
  if (f.plane_count > DmabufFrame::kMaxPlanes) f.plane_count = DmabufFrame::kMaxPlanes;
  std::uint64_t total = 0;
  for (std::uint32_t i = 0; i < f.plane_count; ++i) {
    f.planes[i].stride = gbm_bo_get_stride_for_plane(bo, static_cast<int>(i));
    f.planes[i].offset = gbm_bo_get_offset(bo, static_cast<int>(i));
    const std::uint64_t end = f.planes[i].offset +
                              static_cast<std::uint64_t>(f.planes[i].stride) * height;
    if (end > total) total = end;
  }
  f.mem_size = total;
  f.modifier_tiled = f.modifier != DRM_FORMAT_MOD_LINEAR && f.modifier != DRM_FORMAT_MOD_INVALID;
  f.host_mapped = true;  // GBM_BO_USE_LINEAR: lock() can map it

  log::d(kTag, "allocate {}x{} fourcc={:#x} mod={:#x} stride={} fd={}", width, height, fourcc,
         f.modifier, f.planes[0].stride, fd);
  return hb;
}

std::unique_ptr<HardwareBuffer> HardwareBuffer::import(const DmabufFrame& src, std::string* error) {
  if (src.fd < 0) { if (error) *error = "import: invalid dma-buf fd"; return nullptr; }
  // Own our own fd so lifetimes are independent of the decoder's (it may recycle/close its surface).
  // F_DUPFD_CLOEXEC dups the *description* — same underlying dma-buf memory, no pixel copy.
  const int dup = ::fcntl(src.fd, F_DUPFD_CLOEXEC, 0);
  if (dup < 0) { if (error) *error = "import: F_DUPFD_CLOEXEC failed"; return nullptr; }

  auto hb = std::unique_ptr<HardwareBuffer>(new HardwareBuffer());
  hb->bo_ = nullptr;  // imported: no gbm_bo (the fd already IS a dma-buf); ~HardwareBuffer just closes the fd
  hb->frame_ = src;
  hb->frame_.fd = dup;
  // desc_ carries width/height for describe(); format is best-effort (imported frames may be YUV/NV12,
  // outside HbFormat's RGBA set — the engine's EGLImage import reads frame_.drm_format directly, not this).
  hb->desc_ = HbDesc{src.width, src.height, HbFormat::R8G8B8A8_UNORM, 0};
  log::d(kTag, "import {}x{} fourcc={:#x} mod={:#x} planes={} fd={} (dup {})", src.width, src.height,
         src.drm_format, src.modifier, src.plane_count, src.fd, dup);
  return hb;
}

HardwareBuffer::~HardwareBuffer() {
  if (map_data_ != nullptr) unlock();
  if (frame_.fd >= 0) ::close(frame_.fd);
  if (bo_ != nullptr) gbm_bo_destroy(static_cast<gbm_bo*>(bo_));
}

void* HardwareBuffer::lock(std::uint32_t* stride, std::string* error) {
  if (bo_ == nullptr) { if (error) *error = "imported buffer is not CPU-mappable"; return nullptr; }
  if (map_data_ != nullptr) { if (error) *error = "already locked"; return nullptr; }
  std::uint32_t s = 0;
  void* map_data = nullptr;
  void* p = gbm_bo_map(static_cast<gbm_bo*>(bo_), 0, 0, desc_.width, desc_.height,
                       GBM_BO_TRANSFER_READ_WRITE, &s, &map_data);
  if (p == nullptr || p == MAP_FAILED) { if (error) *error = "gbm_bo_map failed"; return nullptr; }
  map_data_ = map_data;
  if (stride != nullptr) *stride = s;
  return p;
}

void HardwareBuffer::unlock() {
  if (map_data_ == nullptr) return;
  gbm_bo_unmap(static_cast<gbm_bo*>(bo_), map_data_);
  map_data_ = nullptr;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

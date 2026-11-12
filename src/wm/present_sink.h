// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md, S2). The acl-side
// consumer of the dma-buf frames the art-host render process produces and sends
// over acl_ipc. No GPL-3.0 sources.
#ifndef ACL_WM_PRESENT_SINK_H
#define ACL_WM_PRESENT_SINK_H

#include <cstdint>
#include <memory>

namespace acl::wm {

// An externally-rendered dma-buf frame arriving from the render process
// (art-host) over acl_ipc — the split-process analog of what
// gfx::VkRenderTarget produces in-process. The fd is borrowed (owned by the
// caller / the IPC layer); a sink that must retain the buffer past present()
// dups it.
struct ExternalFrame {
  int fd = -1;  // dma-buf fd
  std::uint32_t buffer_id =
      0;  // producer's ring-slot id (a scanout sink caches its FB by this)
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t drm_format = 0;  // fourcc (e.g. XR24)
  std::uint64_t modifier = 0;    // DRM format modifier (0 == LINEAR)
  std::uint32_t stride = 0;
  std::uint32_t offset = 0;
  std::int32_t damage_x = 0, damage_y = 0, damage_w = 0, damage_h = 0;
  int acquire_fence = -1;  // -1 if none; the sink waits it before scanning out
};

// Presents externally-rendered dma-buf frames. Two real backends land behind
// this seam:
//  - Wayland (zwp_linux_dmabuf_v1): import the fd into a wl_buffer and attach
//  it — the desktop path,
//    reusing wm/dmabuf_presenter's import (works alongside a running
//    compositor, e.g. the Pi's labwc).
//  - drm-cxx KMS scanout: drm::fmt::ScanoutBuffer::import_dmabuf(fd,
//  {w,h,fourcc,modifier,{stride,offset}})
//    + drm::present::ScanoutBackend::present() — a direct atomic commit to the
//    display, no compositor. This is the appliance / automotive present path
//    (ADR-0004, plan.md drm-cxx allocator lineage), the natural fit for an
//    in-car/embedded display running acl as the sole client.
// Null records frames and closes fds — it brings up and tests the IPC present
// plumbing before either display backend is wired (mirrors the runtime Null
// VM).
class PresentSink {
 public:
  virtual ~PresentSink() = default;
  PresentSink(const PresentSink&) = delete;
  PresentSink& operator=(const PresentSink&) = delete;

  // Present one frame. Returns false on a non-recoverable error. Takes
  // ownership of neither fd nor acquire_fence unless it dups them; the caller
  // closes them after present() returns.
  virtual bool present(const ExternalFrame& frame) = 0;

 protected:
  PresentSink() = default;
};

enum class PresentBackend {
  Null,        // records + closes (plumbing bring-up)
  Wayland,     // zwp_linux_dmabuf_v1 import (desktop; wm/dmabuf_presenter)
  DrmScanout,  // drm-cxx KMS atomic scanout (appliance/automotive)
};

// Create a present sink. Falls back to Null when the requested backend is not
// compiled/available (e.g. DrmScanout without the drm-cxx dependency), so the
// split-process present path always links.
std::unique_ptr<PresentSink> make_present_sink(PresentBackend backend);

// Backend selected from the ACL_PRESENT_SINK env var
// ("null"|"wayland"|"drm-scanout"); default Null until a display backend is
// wired.
PresentBackend default_present_backend() noexcept;

}  // namespace acl::wm

#endif  // ACL_WM_PRESENT_SINK_H

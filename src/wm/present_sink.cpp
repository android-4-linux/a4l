// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md, S2). No GPL-3.0 sources.

#include "wm/present_sink.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "acl_config.h"
#include "log/log.h"

namespace acl::wm {

#if ACL_HAVE_DRM_CXX
std::unique_ptr<PresentSink> make_drm_scanout_sink();  // drm_scanout_sink.cpp
#endif

namespace {

constexpr std::string_view kTag = "acl.wm.present";

// Records frame metadata and closes fds. Brings up and tests the split-process present path (IPC
// PresentFrame -> sink) before the Wayland / drm-cxx-scanout display backends are wired.
class NullPresentSink final : public PresentSink {
 public:
  bool present(const ExternalFrame& f) override {
    log::i(kTag,
           "present(null): dma-buf fd={} {}x{} fourcc={:#010x} mod={:#018x} stride={} damage=({},{} {}x{})",
           f.fd, f.width, f.height, f.drm_format, f.modifier, f.stride, f.damage_x, f.damage_y,
           f.damage_w, f.damage_h);
    // The IPC layer hands us borrowed fds; a display backend would import them, but Null just drops.
    if (f.fd >= 0) ::close(f.fd);
    if (f.acquire_fence >= 0) ::close(f.acquire_fence);
    return true;
  }
};

}  // namespace

PresentBackend default_present_backend() noexcept {
  if (const char* e = std::getenv("ACL_PRESENT_SINK")) {
    if (std::strcmp(e, "wayland") == 0) return PresentBackend::Wayland;
    if (std::strcmp(e, "drm-scanout") == 0) return PresentBackend::DrmScanout;
  }
  return PresentBackend::Null;
}

std::unique_ptr<PresentSink> make_present_sink(PresentBackend backend) {
  switch (backend) {
    case PresentBackend::Wayland:
      // TODO(S2): wrap wm/dmabuf_presenter's zwp_linux_dmabuf_v1 import for external fds.
      log::w(kTag, "present sink 'wayland' not yet wired; using null");
      return std::make_unique<NullPresentSink>();
    case PresentBackend::DrmScanout:
#if ACL_HAVE_DRM_CXX
      if (auto s = make_drm_scanout_sink()) return s;
      log::w(kTag, "drm-scanout sink unavailable (no DRM master / output); using null");
#else
      log::w(kTag, "present sink 'drm-scanout' needs the drm-cxx dependency; using null");
#endif
      return std::make_unique<NullPresentSink>();
    case PresentBackend::Null:
    default:
      return std::make_unique<NullPresentSink>();
  }
}

}  // namespace acl::wm

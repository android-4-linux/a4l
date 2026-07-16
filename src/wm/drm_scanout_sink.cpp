// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md, S2). Uses drm-cxx (MIT,
// app/drm-cxx) drm::fmt::ScanoutBuffer::import_dmabuf for the zero-copy, modifier-preserving FB;
// the KMS output-pick + atomic commit are adapted from drm-cxx's MIT examples/common/kms_present.hpp
// (pick_target / commit_fb). No GPL-3.0 sources.
//
// The appliance/automotive present path: an art-host-rendered dma-buf (arriving over acl_ipc) is
// scanned out straight to the display — no compositor. Optimal path: the producer's *tiled* modifier
// is imported as-is (no CPU copy, no detile), verified against the primary plane by a TEST_ONLY
// commit, then page-flipped; the producer's acquire fence gates the flip (IN_FENCE_FD, explicit
// sync); FBs are cached per buffer-ring slot (PresentFrame.buffer_id) so a steady stream re-flips
// without re-importing.

#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <drm-cxx/fmt/format_mod.hpp>

#include "log/log.h"
#include "wm/present_sink.h"

namespace acl::wm {
namespace {

constexpr std::string_view kTag = "acl.wm.drm";

std::uint32_t prop_id(int fd, std::uint32_t obj, std::uint32_t type, const char* name) {
  drmModeObjectProperties* p = drmModeObjectGetProperties(fd, obj, type);
  std::uint32_t id = 0;
  for (std::uint32_t i = 0; p && i < p->count_props; ++i) {
    drmModePropertyRes* pr = drmModeGetProperty(fd, p->props[i]);
    if (pr) {
      if (std::strcmp(pr->name, name) == 0) id = pr->prop_id;
      drmModeFreeProperty(pr);
    }
  }
  if (p) drmModeFreeObjectProperties(p);
  return id;
}

// The picked output: connector + CRTC + mode + primary plane. (Adapted from kms_present.hpp.)
struct Target {
  std::uint32_t connector_id = 0;
  std::uint32_t crtc_id = 0;
  std::uint32_t crtc_index = 0;
  std::uint32_t primary_plane = 0;
  drmModeModeInfo mode{};
};

std::optional<Target> pick_target(int fd) {
  drmModeRes* res = drmModeGetResources(fd);
  if (!res) return std::nullopt;
  Target t;
  drmModeConnector* conn = nullptr;
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      conn = c;
      break;
    }
    if (c) drmModeFreeConnector(c);
  }
  if (!conn) {
    drmModeFreeResources(res);
    return std::nullopt;
  }
  t.connector_id = conn->connector_id;
  t.mode = conn->modes[0];  // preferred
  for (int e = 0; e < conn->count_encoders && t.crtc_id == 0; ++e) {
    drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[e]);
    if (enc) {
      for (int i = 0; i < res->count_crtcs; ++i) {
        if (enc->possible_crtcs & (1u << i)) {
          t.crtc_id = res->crtcs[i];
          t.crtc_index = static_cast<std::uint32_t>(i);
          break;
        }
      }
      drmModeFreeEncoder(enc);
    }
  }
  drmModeFreeConnector(conn);
  if (t.crtc_id == 0) {
    drmModeFreeResources(res);
    return std::nullopt;
  }
  drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
  for (std::uint32_t i = 0; pr && i < pr->count_planes && t.primary_plane == 0; ++i) {
    drmModePlane* pl = drmModeGetPlane(fd, pr->planes[i]);
    if (pl && (pl->possible_crtcs & (1u << t.crtc_index))) {
      const std::uint32_t type = prop_id(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "type");
      drmModeObjectProperties* p =
          drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
      for (std::uint32_t k = 0; p && k < p->count_props; ++k)
        if (p->props[k] == type && p->prop_values[k] == DRM_PLANE_TYPE_PRIMARY)
          t.primary_plane = pl->plane_id;
      if (p) drmModeFreeObjectProperties(p);
    }
    if (pl) drmModeFreePlane(pl);
  }
  if (pr) drmModeFreePlaneResources(pr);
  drmModeFreeResources(res);
  return t.primary_plane ? std::optional<Target>(t) : std::nullopt;
}

// Atomic modeset + plane FB, with the producer's acquire fence gating scanout (IN_FENCE_FD).
int commit_fb(int fd, const Target& t, std::uint32_t fb_id, int in_fence, std::uint32_t flags) {
  const std::uint32_t w = t.mode.hdisplay, h = t.mode.vdisplay;
  std::uint32_t mode_blob = 0;
  drmModeModeInfo mode = t.mode;
  drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &mode_blob);
  const auto PL = DRM_MODE_OBJECT_PLANE, CR = DRM_MODE_OBJECT_CRTC, CO = DRM_MODE_OBJECT_CONNECTOR;
  auto P = [&](std::uint32_t o, std::uint32_t ty, const char* n) { return prop_id(fd, o, ty, n); };

  drmModeAtomicReq* req = drmModeAtomicAlloc();
  drmModeAtomicAddProperty(req, t.crtc_id, P(t.crtc_id, CR, "MODE_ID"), mode_blob);
  drmModeAtomicAddProperty(req, t.crtc_id, P(t.crtc_id, CR, "ACTIVE"), 1);
  drmModeAtomicAddProperty(req, t.connector_id, P(t.connector_id, CO, "CRTC_ID"), t.crtc_id);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "FB_ID"), fb_id);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_ID"), t.crtc_id);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_X"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_Y"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_W"), std::uint64_t(w) << 16);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_H"), std::uint64_t(h) << 16);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_X"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_Y"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_W"), w);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_H"), h);
  if (in_fence >= 0) {
    const std::uint32_t inf = P(t.primary_plane, PL, "IN_FENCE_FD");
    if (inf) drmModeAtomicAddProperty(req, t.primary_plane, inf, static_cast<std::uint64_t>(in_fence));
  }
  int r = drmModeAtomicCommit(fd, req, flags, nullptr);
  drmModeAtomicFree(req);
  if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);
  return r;
}

class DrmScanoutSink final : public PresentSink {
 public:
  static std::unique_ptr<DrmScanoutSink> create() {
    const char* dev = std::getenv("ACL_DRM_DEVICE");
    const char* path = (dev && *dev) ? dev : "/dev/dri/card0";
    int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      log::e(kTag, "open({}): {}", path, std::strerror(errno));
      return nullptr;
    }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    auto tgt = pick_target(fd);
    if (!tgt) {
      log::e(kTag, "no connected output/primary plane on {} (is a compositor holding DRM master?)", path);
      ::close(fd);
      return nullptr;
    }
    log::i(kTag, "scanout on {}: {}x{}@{} crtc={} plane={}", path, tgt->mode.hdisplay,
           tgt->mode.vdisplay, tgt->mode.vrefresh, tgt->crtc_id, tgt->primary_plane);
    return std::unique_ptr<DrmScanoutSink>(new DrmScanoutSink(fd, *tgt));
  }

  bool present(const ExternalFrame& f) override {
    // Cache the imported FB per buffer-ring slot so a steady stream re-flips without re-importing.
    drm::fmt::ScanoutBuffer* fb = nullptr;
    if (auto it = fbs_.find(f.buffer_id); it != fbs_.end()) {
      fb = &it->second;
      if (f.fd >= 0) ::close(f.fd);  // same slot: keep the cached import, drop the new fd
    } else {
      drm::fmt::ScanoutBuffer::ImportDesc d;
      d.width = f.width;
      d.height = f.height;
      d.fourcc = f.drm_format;
      d.modifier = drm::fmt::Modifier{f.modifier};
      drm::fmt::ScanoutBuffer::ImportDesc::Plane pl;
      pl.dmabuf_fd = f.fd;
      pl.stride = f.stride;
      pl.offset = f.offset;
      const std::array<drm::fmt::ScanoutBuffer::ImportDesc::Plane, 1> planes{pl};
      d.planes = planes;
      auto imported = drm::fmt::ScanoutBuffer::import_dmabuf(fd_, d);
      if (f.fd >= 0) ::close(f.fd);  // import_dmabuf dups the fd
      if (!imported) {
        log::e(kTag, "import_dmabuf: {}", imported.error().message());
        if (f.acquire_fence >= 0) ::close(f.acquire_fence);
        return false;
      }
      fb = &fbs_.emplace(f.buffer_id, std::move(*imported)).first->second;
    }

    std::uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
    if (first_) flags = DRM_MODE_ATOMIC_ALLOW_MODESET;  // first commit modesets, must block
    int r = commit_fb(fd_, target_, fb->fb_id(), f.acquire_fence, flags);
    if (f.acquire_fence >= 0) ::close(f.acquire_fence);  // the kernel dups IN_FENCE_FD
    if (r != 0) {
      log::e(kTag, "atomic commit: {}", std::strerror(-r));
      return false;
    }
    first_ = false;
    return true;
  }

  ~DrmScanoutSink() override {
    fbs_.clear();
    if (fd_ >= 0) ::close(fd_);
  }

 private:
  DrmScanoutSink(int fd, const Target& t) : fd_(fd), target_(t) {}
  int fd_;
  Target target_;
  bool first_ = true;
  std::unordered_map<std::uint64_t, drm::fmt::ScanoutBuffer> fbs_;
};

}  // namespace

std::unique_ptr<PresentSink> make_drm_scanout_sink() { return DrmScanoutSink::create(); }

}  // namespace acl::wm

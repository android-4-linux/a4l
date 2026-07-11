// SPDX-License-Identifier: Apache-2.0
// Provenance: wayland-cxx-scanner (MIT) — its scanner-generated protocol headers
// and header-only wl/ CRTP framework (proxy_impl/registry/display/xdg_shell), and
// the structure of its presentation-shm example. Wayland + xdg-shell protocols
// (freedesktop, MIT). No GPL-3.0 sources.
//
// The real Wayland presenter (compiled only when ACLCXX_HAVE_WCS): upload an
// aclcxx::gfx::Surface into a wl_shm buffer and show it via wcs CRTP bindings —
// connect -> registry -> bind(wl_compositor/wl_shm/xdg_wm_base) -> wl_surface +
// xdg_toplevel -> shm buffer -> attach/commit -> block on the wl_surface.frame
// callback. One-shot; the surface is torn down on return.
//
// wcs lifetime rule: handler objects must have a stable address for their bound
// lifetime (the dispatcher is registered by address). All handlers are direct
// members of the non-movable Presenter, so their addresses never change.

#include "wm/present.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

// Generated protocol headers FIRST (define traits + CRTP proxy classes), then the
// wl/ framework headers that extend them (interface tables, xdg handlers).
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/proxy_impl.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

#include "log/log.h"

namespace aclcxx::wm {
namespace {

using namespace wayland::client;
using namespace xdg_shell::client;

constexpr const char* kTag = "aclcxx.wm";

// Event-less interfaces (wl_compositor, wl_shm_pool) have no generated _Dispatch,
// so they use WlPtr::Attach; interfaces with events use _SetProxy (BindHandler /
// SetupHandler).
class CompositorH : public CWlCompositor<CompositorH> {};
class ShmPoolH : public CWlShmPool<ShmPoolH> {};
class ShmH : public CWlShm<ShmH> {
 public:
  uint32_t formats = 0;
  void OnFormat(uint32_t f) override { if (f < 32u) formats |= (1u << f); }
};
class SurfaceH : public CWlSurface<SurfaceH> {};
class BufferH : public CWlBuffer<BufferH> {
 public:
  void OnRelease() override {}
};
class FrameH : public CWlCallback<FrameH> {
 public:
  bool* done = nullptr;
  void OnDone(uint32_t) override { if (done != nullptr) *done = true; }
};

// A memfd-backed ARGB_8888 mapping for one wl_shm buffer.
struct ShmMapping {
  int fd = -1;
  void* data = MAP_FAILED;
  std::size_t size = 0;
  ~ShmMapping() { Reset(); }
  bool Create(std::size_t n) noexcept {
    Reset();
    fd = memfd_create("aclcxx-wm", 0);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(n)) < 0) { Reset(); return false; }
    data = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { Reset(); return false; }
    size = n;
    return true;
  }
  void Reset() noexcept {
    if (data != MAP_FAILED) { munmap(data, size); data = MAP_FAILED; }
    if (fd >= 0) { close(fd); fd = -1; }
    size = 0;
  }
};

class Presenter {
 public:
  Presenter() = default;
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  bool Present(const gfx::Surface& surface, std::string_view title,
               std::string_view app_id, int timeout_ms) {
    w_ = surface.width();
    h_ = surface.height();
    if (w_ <= 0 || h_ <= 0) { log::w(kTag, "present: empty surface"); return false; }

    if (!display_.Connect()) { log::w(kTag, "present: no Wayland display"); return false; }
    if (!registry_.Create(display_.Get())) { log::w(kTag, "present: wl_registry unavailable"); return false; }
    registry_.OnGlobal([this](wl::CRegistry&, uint32_t name, std::string_view iface, uint32_t ver) {
      if (iface == wl_compositor_traits::interface_name) { comp_name_ = name; comp_ver_ = ver; }
      else if (iface == wl_shm_traits::interface_name) { shm_name_ = name; shm_ver_ = ver; }
      else if (iface == xdg_wm_base_traits::interface_name) { xdg_name_ = name; xdg_ver_ = ver; }
    });
    if (!wl::RoundtripWithTimeout(display_.Get(), timeout_ms)) { log::w(kTag, "present: registry roundtrip failed"); return false; }
    if (comp_name_ == 0 || shm_name_ == 0 || xdg_name_ == 0) {
      log::w(kTag, "present: compositor missing wl_compositor/wl_shm/xdg_wm_base");
      return false;
    }

    if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(comp_name_, std::min(comp_ver_, wl_compositor_traits::version)))
      comp_.Attach(raw);
    else return false;
    if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) return false;
    if (!wl::BindHandler<xdg_wm_base_traits>(registry_, xdg_wm_base_, xdg_name_, xdg_ver_)) return false;
    if (!wl::RoundtripWithTimeout(display_.Get(), timeout_ms)) return false;  // shm formats arrive

    if (!wl::SetupHandler(surface_, wl::construct<wl_surface_traits, wl_compositor_traits::Op::CreateSurface>(*comp_.Get()))) return false;
    if (!wl::SetupHandler(xdg_surface_, wl::construct<xdg_surface_traits, xdg_wm_base_traits::Op::GetXdgSurface>(*xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) return false;
    xdg_surface_.Get()->app_ = this;
    if (!wl::SetupHandler(xdg_toplevel_, wl::construct<xdg_toplevel_traits, xdg_surface_traits::Op::GetToplevel>(*xdg_surface_.Get()))) return false;
    xdg_toplevel_.Get()->app_ = this;
    title_.assign(title);
    app_id_.assign(app_id);
    xdg_toplevel_.Get()->SetTitle(title_.c_str());
    xdg_toplevel_.Get()->SetAppId(app_id_.c_str());

    if (!MakeBuffer(surface)) { log::w(kTag, "present: shm buffer allocation failed"); return false; }

    frame_.done = &frame_done_;
    surface_.Get()->Commit();  // trigger the configure sequence

    const int fd = wl_display_get_fd(display_.Get());
    while (!(frame_done_ || closed_)) {
      while (wl_display_flush(display_.Get()) < 0 && errno == EAGAIN) {}
      pollfd p{fd, POLLIN, 0};
      const int n = poll(&p, 1, timeout_ms);
      if (n <= 0) { log::w(kTag, "present: timed out waiting for frame callback"); return false; }
      if (wl_display_dispatch(display_.Get()) < 0) { log::w(kTag, "present: dispatch error"); return false; }
    }
    if (closed_) { log::w(kTag, "present: compositor closed the toplevel"); return false; }
    log::i(kTag, "presented {}x{} to Wayland compositor ({}); frame callback fired",
           w_, h_, title_);
    return true;
  }

  // xdg callbacks (invoked by wl::XdgSurfaceHandler / XdgToplevelHandler)
  void OnXdgSurfaceConfigure(uint32_t) {
    if (attached_) return;
    if (wl_proxy* raw = wl::construct<wl_callback_traits, wl_surface_traits::Op::Frame>(*surface_.Get()))
      frame_._SetProxy(raw);
    surface_.Get()->Attach(buffer_.Get()->GetProxy(), 0, 0);
    surface_.Get()->Damage(0, 0, w_, h_);
    surface_.Get()->Commit();
    attached_ = true;
  }
  void OnToplevelConfigure(int32_t, int32_t) {}
  void OnToplevelClose() { closed_ = true; }

 private:
  bool MakeBuffer(const gfx::Surface& surface) {
    const std::size_t stride = static_cast<std::size_t>(w_) * 4u;
    const std::size_t total = stride * static_cast<std::size_t>(h_);
    if (!mem_.Create(total)) return false;
    // gfx::Surface pixels are native-endian 0xAARRGGBB, which is exactly wl_shm
    // ARGB8888 — a straight copy (surface is tightly packed, stride == w*4).
    std::memcpy(mem_.data, surface.data(), total);

    wl_proxy* raw_pool = wl::construct<wl_shm_pool_traits, wl_shm_traits::Op::CreatePool>(*shm_.Get(), mem_.fd, static_cast<int32_t>(total));
    if (raw_pool == nullptr) return false;
    pool_.Attach(raw_pool);
    wl_proxy* raw = wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
        *pool_.Get(), 0, w_, h_, static_cast<int32_t>(stride), WL_SHM_FORMAT_ARGB8888);
    if (raw == nullptr) return false;
    buffer_.Get()->_SetProxy(raw);
    pool_.Reset();  // only needed during buffer creation
    return true;
  }

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<CompositorH> comp_;
  wl::WlPtr<ShmH> shm_;
  wl::WlPtr<ShmPoolH> pool_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<SurfaceH> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<Presenter>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<Presenter>> xdg_toplevel_;
  wl::WlPtr<BufferH> buffer_;
  FrameH frame_;  // direct member: stable address for the dispatcher
  ShmMapping mem_;
  std::string title_, app_id_;

  uint32_t comp_name_ = 0, comp_ver_ = 0, shm_name_ = 0, shm_ver_ = 0, xdg_name_ = 0, xdg_ver_ = 0;
  int w_ = 0, h_ = 0;
  bool attached_ = false, frame_done_ = false, closed_ = false;
};

}  // namespace

bool wayland_available() noexcept {
  wl::DisplayHandle d;
  return d.Connect();  // disconnects on destruction
}

bool present_surface(const gfx::Surface& surface, std::string_view title,
                     std::string_view app_id, int timeout_ms) noexcept {
  Presenter presenter;
  return presenter.Present(surface, title, app_id, timeout_ms);
}

}  // namespace aclcxx::wm

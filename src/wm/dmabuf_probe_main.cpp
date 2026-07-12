// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §2, ADR-4/ADR-6; wcs example skia-vulkan-dmabuf (behavioral).
// No GPL-3.0 sources.
//
// P2.4/V2b live probe: render a demo scene on the Ganesh Vulkan render target and
// present it to the Wayland compositor as a Linux dma-buf (zwp_linux_dmabuf_v1). The
// render callback prefers the modifier-tiled direct path (Skia draws straight into the
// dma-buf) using the compositor's advertised modifiers, falling back to the linear
// copy path. Run against a live compositor:  dmabuf_probe [frames] [title].

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "acl_config.h"

#if ACL_HAVE_WCS && ACL_HAVE_VULKAN

#include "gfx/gfx_vk.h"
#include "wm/present.h"

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 3;
  const std::string title = argc > 2 ? argv[2] : "acl dma-buf";

  // A modest window; a background clear + one filled rect (the V1/V2 demo scene).
  constexpr int kW = 480, kH = 320;
  constexpr std::uint32_t kBg = 0xFF204060, kFg = 0xFFCC8844;
  constexpr int kL = 80, kT = 60, kR = 400, kB = 260;

  std::string err;
  auto target = acl::gfx::VkRenderTarget::Create(kW, kH, &err);
  if (!target) {
    std::fprintf(stderr, "dmabuf_probe: no Vulkan render target: %s\n", err.c_str());
    return 2;
  }
  std::fprintf(stderr, "dmabuf_probe: GPU %s\n", target->device_name().c_str());

  acl::wm::DmabufRenderFn render =
      [&](const std::vector<std::uint64_t>& advertised,
          acl::gfx::DmabufFrame* out) -> bool {
    std::string e;
    // Prefer the zero-copy modifier-tiled direct path when the compositor advertised
    // modifiers this GPU can render into; else the linear copy fallback.
    if (target->modifier_tiled_available() && !advertised.empty() &&
        target->render_to_modifier_dmabuf(advertised, kBg, kL, kT, kR, kB, kFg, out, &e)) {
      return true;
    }
    if (target->render_and_export_linear(kBg, kL, kT, kR, kB, kFg, out, &e)) return true;
    std::fprintf(stderr, "dmabuf_probe: render failed: %s\n", e.c_str());
    return false;
  };

  const bool ok = acl::wm::present_dmabuf(render, title, "com.acl.dmabuf", frames);
  std::fprintf(stderr, "dmabuf_probe: %s\n", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}

#else
int main() {
  std::fprintf(stderr, "dmabuf_probe: built without wcs+vulkan; nothing to do\n");
  return 0;
}
#endif

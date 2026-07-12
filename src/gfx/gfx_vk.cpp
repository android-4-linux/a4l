// SPDX-License-Identifier: Apache-2.0
// Provenance: wcs example skia-vulkan-dmabuf/main.cpp (Apache-2.0/MIT) — the
// InitVulkan pattern (instance 1.1, physical-device pick, graphics queue,
// VulkanMemoryAllocators::Make, GrDirectContexts::MakeVulkan); Skia public API
// (include/gpu/ganesh/vk/GrVkDirectContext.h, include/gpu/vk/VulkanBackendContext.h,
// include/gpu/ganesh/SkSurfaceGanesh.h) + the VulkanMemoryAllocatorPriv Make; ADR-4
// (single pinned Skia, hwui dropped). No GPL-3.0 sources.

#include "gfx/gfx_vk.h"

#if ACL_HAVE_VULKAN

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Use the ResultValue (.result/.value) API — no C++ exceptions — as the wcs example
// does; must precede <vulkan/vulkan.hpp>.
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>

#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanMemoryAllocator.h"
// Private headers: the bundled VMA factory + the ThreadSafe enum it takes (the same
// includes the wcs example uses; pinned to the m150 build, ADR-4).
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"

namespace acl::gfx {

namespace {
// gfx::Surface stores 0xAARRGGBB; little-endian byte order B,G,R,A = kBGRA_8888,
// matching kVkFormat below (the same choice the wcs example makes).
constexpr SkColorType kSkColorType = kBGRA_8888_SkColorType;
}  // namespace

struct VkRenderTarget::Impl {
  vk::UniqueInstance instance;
  vk::PhysicalDevice phys_dev;
  vk::UniqueDevice device;
  vk::Queue queue;
  std::uint32_t queue_family = UINT32_MAX;
  std::vector<const char*> device_exts;
  skgpu::VulkanExtensions vk_extensions;
  sk_sp<skgpu::VulkanMemoryAllocator> allocator;
  sk_sp<GrDirectContext> context;
  sk_sp<SkSurface> surface;
};

VkRenderTarget::VkRenderTarget() : impl_(std::make_unique<Impl>()) {}
VkRenderTarget::~VkRenderTarget() {
  if (impl_ && impl_->context) impl_->context->flushAndSubmit(GrSyncCpu::kYes);
}

std::unique_ptr<VkRenderTarget> VkRenderTarget::Create(int width, int height,
                                                       std::string* error) {
  const auto fail = [&](std::string_view msg) -> std::unique_ptr<VkRenderTarget> {
    if (error) *error = std::string(msg);
    return nullptr;
  };
  if (width <= 0 || height <= 0) return fail("non-positive size");

  auto rt = std::unique_ptr<VkRenderTarget>(new VkRenderTarget());
  rt->width_ = width;
  rt->height_ = height;
  Impl& vk_ = *rt->impl_;

  // ── Instance (Vulkan 1.1 — Skia's minimum) ─────────────────────────────────
  constexpr vk::ApplicationInfo app_info{"acl-gfx", VK_MAKE_VERSION(0, 1, 0),
                                         nullptr, 0, VK_API_VERSION_1_1};
  const vk::InstanceCreateInfo inst_ci{{}, &app_info};
  auto inst_rv = vk::createInstanceUnique(inst_ci);
  if (inst_rv.result != vk::Result::eSuccess) return fail("vkCreateInstance failed");
  vk_.instance = std::move(inst_rv.value);

  // ── Physical device (prefer a real GPU over a CPU/llvmpipe renderer) ────────
  auto phys_rv = vk_.instance->enumeratePhysicalDevices();
  if (phys_rv.result != vk::Result::eSuccess || phys_rv.value.empty())
    return fail("no Vulkan physical devices");
  vk_.phys_dev = phys_rv.value.front();
  for (const vk::PhysicalDevice& pd : phys_rv.value) {
    const vk::PhysicalDeviceType t = pd.getProperties().deviceType;
    if (t == vk::PhysicalDeviceType::eDiscreteGpu ||
        t == vk::PhysicalDeviceType::eIntegratedGpu) {
      vk_.phys_dev = pd;
      break;
    }
  }
  rt->device_name_ = vk_.phys_dev.getProperties().deviceName.data();

  // ── Graphics queue family ──────────────────────────────────────────────────
  const auto qfps = vk_.phys_dev.getQueueFamilyProperties();
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(qfps.size()); ++i) {
    if (qfps.at(i).queueFlags & vk::QueueFlagBits::eGraphics) {
      vk_.queue_family = i;
      break;
    }
  }
  if (vk_.queue_family == UINT32_MAX) return fail("no graphics queue family");

  // ── Logical device (no device extensions needed for the offscreen V1 path;
  //     dma-buf export extensions arrive in V2) ────────────────────────────────
  constexpr float queue_prio = 1.0f;
  const vk::DeviceQueueCreateInfo queue_ci{{}, vk_.queue_family, 1, &queue_prio};
  const vk::DeviceCreateInfo dev_ci{{}, 1, &queue_ci};
  auto dev_rv = vk_.phys_dev.createDeviceUnique(dev_ci);
  if (dev_rv.result != vk::Result::eSuccess) return fail("vkCreateDevice failed");
  vk_.device = std::move(dev_rv.value);
  vk_.queue = vk_.device->getQueue(vk_.queue_family, 0);

  // ── Skia Ganesh Vulkan context ─────────────────────────────────────────────
  const skgpu::VulkanGetProc get_proc =
      [](const char* name, VkInstance instance,
         VkDevice device) -> PFN_vkVoidFunction {
    if (device != VK_NULL_HANDLE) return vkGetDeviceProcAddr(device, name);
    return vkGetInstanceProcAddr(instance, name);
  };
  vk_.vk_extensions.init(get_proc, *vk_.instance, vk_.phys_dev, 0, nullptr,
                         static_cast<std::uint32_t>(vk_.device_exts.size()),
                         vk_.device_exts.data());

  skgpu::VulkanBackendContext backend{};
  backend.fInstance = *vk_.instance;
  backend.fPhysicalDevice = vk_.phys_dev;
  backend.fDevice = *vk_.device;
  backend.fQueue = vk_.queue;
  backend.fGraphicsQueueIndex = vk_.queue_family;
  backend.fMaxAPIVersion = VK_API_VERSION_1_1;
  backend.fVkExtensions = &vk_.vk_extensions;
  backend.fGetProc = get_proc;
  vk_.allocator = skgpu::VulkanMemoryAllocators::Make(backend, skgpu::ThreadSafe::kNo);
  if (!vk_.allocator) return fail("VulkanMemoryAllocators::Make failed");
  backend.fMemoryAllocator = vk_.allocator;

  vk_.context = GrDirectContexts::MakeVulkan(backend);
  if (!vk_.context) return fail("GrDirectContexts::MakeVulkan failed");

  // ── Offscreen GPU render target ─────────────────────────────────────────────
  // GPU render targets require premultiplied alpha (a Ganesh constraint, and what
  // the wcs example uses); readback below converts to the gfx unpremul convention.
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kSkColorType, kPremul_SkAlphaType);
  vk_.surface = SkSurfaces::RenderTarget(vk_.context.get(), skgpu::Budgeted::kYes, info);
  if (!vk_.surface) return fail("SkSurfaces::RenderTarget (Vulkan) failed");

  return rt;
}

bool VkRenderTarget::render_rect(Surface& out, std::uint32_t bg_argb, int left,
                                 int top, int right, int bottom,
                                 std::uint32_t rect_argb) {
  Impl& vk_ = *impl_;
  if (!vk_.surface || !vk_.context) return false;
  if (out.width() != width_ || out.height() != height_) return false;

  SkCanvas* canvas = vk_.surface->getCanvas();
  canvas->clear(static_cast<SkColor>(bg_argb));
  SkPaint paint;
  paint.setColor(static_cast<SkColor>(rect_argb));
  paint.setAntiAlias(false);  // deterministic; matches the software rasterizer
  canvas->drawRect(SkRect::MakeLTRB(static_cast<SkScalar>(left), static_cast<SkScalar>(top),
                                    static_cast<SkScalar>(right), static_cast<SkScalar>(bottom)),
                   paint);

  // Flush GPU work and block until it completes, then read the pixels back.
  vk_.context->flushAndSubmit(GrSyncCpu::kYes);

  const SkImageInfo info =
      SkImageInfo::Make(width_, height_, kSkColorType, kUnpremul_SkAlphaType);
  std::vector<std::uint32_t> buf(static_cast<std::size_t>(width_) *
                                 static_cast<std::size_t>(height_));
  if (!vk_.surface->readPixels(info, buf.data(),
                               static_cast<std::size_t>(width_) * sizeof(std::uint32_t),
                               0, 0)) {
    return false;
  }
  out.blit(buf.data());
  return true;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_VULKAN

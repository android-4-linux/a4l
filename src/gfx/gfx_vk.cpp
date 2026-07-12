// SPDX-License-Identifier: Apache-2.0
// Provenance: wcs example skia-vulkan-dmabuf/main.cpp (Apache-2.0/MIT) — the
// InitVulkan pattern (instance 1.1, physical-device pick, graphics queue,
// VulkanMemoryAllocators::Make, GrDirectContexts::MakeVulkan); Skia public API
// (include/gpu/ganesh/vk/GrVkDirectContext.h, include/gpu/vk/VulkanBackendContext.h,
// include/gpu/ganesh/SkSurfaceGanesh.h) + the VulkanMemoryAllocatorPriv Make; ADR-4
// (single pinned Skia, hwui dropped). No GPL-3.0 sources.

#include "gfx/gfx_vk.h"

#if ACL_HAVE_VULKAN

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <unistd.h>  // close (owns the exported dma-buf fd)

#include <drm_fourcc.h>  // DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR

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
#include "include/gpu/MutableTextureState.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"  // kTopLeft_GrSurfaceOrigin
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanMemoryAllocator.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"
// Private headers: the bundled VMA factory + the ThreadSafe enum it takes (the same
// includes the wcs example uses; pinned to the m150 build, ADR-4).
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"

namespace acl::gfx {

namespace {
// gfx::Surface stores 0xAARRGGBB; little-endian byte order B,G,R,A = kBGRA_8888,
// matching kVkFormat below (the same choice the wcs example makes).
constexpr SkColorType kSkColorType = kBGRA_8888_SkColorType;
constexpr vk::Format kVkFormat = vk::Format::eB8G8R8A8Unorm;
constexpr std::uint32_t kDrmFormat = DRM_FORMAT_XRGB8888;  // opaque; matches kVkFormat
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

  // dma-buf export (V2): the linear exported image + its command pool/fence.
  bool dmabuf_export_ok = false;
  vk::UniqueCommandPool cmd_pool;
  vk::UniqueImage export_image;
  vk::UniqueDeviceMemory export_memory;
  vk::UniqueCommandBuffer copy_cmd;
  vk::UniqueFence copy_fence;
  int export_fd = -1;
  DmabufFrame frame{};  // last exported descriptor
};

VkRenderTarget::VkRenderTarget() : impl_(std::make_unique<Impl>()) {}
VkRenderTarget::~VkRenderTarget() {
  if (impl_) {
    if (impl_->context) impl_->context->flushAndSubmit(GrSyncCpu::kYes);
    if (impl_->export_fd >= 0) ::close(impl_->export_fd);
  }
}

bool VkRenderTarget::dmabuf_export_available() const {
  return impl_ && impl_->dmabuf_export_ok;
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

  // ── Device extensions: dma-buf export (V2). Their absence just disables the
  //     dma-buf path (V1 offscreen render + readback still works). ─────────────
  auto ext_rv = vk_.phys_dev.enumerateDeviceExtensionProperties();
  if (ext_rv.result == vk::Result::eSuccess) {
    const auto has_ext = [&](std::string_view name) {
      return std::any_of(ext_rv.value.begin(), ext_rv.value.end(),
                         [&](const vk::ExtensionProperties& e) {
                           return std::string_view{e.extensionName.data()} == name;
                         });
    };
    if (has_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
        has_ext(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
      vk_.device_exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
      vk_.device_exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
      vk_.dmabuf_export_ok = true;
    }
  }

  // ── Logical device ─────────────────────────────────────────────────────────
  constexpr float queue_prio = 1.0f;
  const vk::DeviceQueueCreateInfo queue_ci{{}, vk_.queue_family, 1, &queue_prio};
  const vk::DeviceCreateInfo dev_ci{{}, 1, &queue_ci, 0, nullptr,
                                    static_cast<std::uint32_t>(vk_.device_exts.size()),
                                    vk_.device_exts.data()};
  auto dev_rv = vk_.phys_dev.createDeviceUnique(dev_ci);
  if (dev_rv.result != vk::Result::eSuccess) return fail("vkCreateDevice failed");
  vk_.device = std::move(dev_rv.value);
  vk_.queue = vk_.device->getQueue(vk_.queue_family, 0);

  // Command pool for the dma-buf copy pass (V2).
  if (vk_.dmabuf_export_ok) {
    const vk::CommandPoolCreateInfo pool_ci{
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer, vk_.queue_family};
    auto rv = vk_.device->createCommandPoolUnique(pool_ci);
    if (rv.result == vk::Result::eSuccess)
      vk_.cmd_pool = std::move(rv.value);
    else
      vk_.dmabuf_export_ok = false;
  }

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
  // kTopLeft origin so the VkImage memory is stored top-down: RenderTarget defaults
  // to kBottomLeft, which readPixels hides but a raw vkCmdCopyImage into the dma-buf
  // (V2) would copy verbatim — i.e. vertically flipped. Wayland surfaces are top-left
  // origin too, so top-down is the correct storage for both readback and present.
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kSkColorType, kPremul_SkAlphaType);
  vk_.surface = SkSurfaces::RenderTarget(vk_.context.get(), skgpu::Budgeted::kYes, info,
                                         /*sampleCount=*/0, kTopLeft_GrSurfaceOrigin,
                                         /*surfaceProps=*/nullptr);
  if (!vk_.surface) return fail("SkSurfaces::RenderTarget (Vulkan) failed");

  return rt;
}

namespace {
// The V2 scene stand-in (mirrors gfx_skia.cpp / render_rect): a background clear
// plus one AA-off filled rect, so GPU output is bit-comparable to the software path.
void draw_scene(SkCanvas* canvas, std::uint32_t bg_argb, int left, int top, int right,
                int bottom, std::uint32_t rect_argb) {
  canvas->clear(static_cast<SkColor>(bg_argb));
  SkPaint paint;
  paint.setColor(static_cast<SkColor>(rect_argb));
  paint.setAntiAlias(false);
  canvas->drawRect(SkRect::MakeLTRB(static_cast<SkScalar>(left), static_cast<SkScalar>(top),
                                    static_cast<SkScalar>(right), static_cast<SkScalar>(bottom)),
                   paint);
}
}  // namespace

bool VkRenderTarget::render_rect(Surface& out, std::uint32_t bg_argb, int left,
                                 int top, int right, int bottom,
                                 std::uint32_t rect_argb) {
  Impl& vk_ = *impl_;
  if (!vk_.surface || !vk_.context) return false;
  if (out.width() != width_ || out.height() != height_) return false;

  draw_scene(vk_.surface->getCanvas(), bg_argb, left, top, right, bottom, rect_argb);

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

// Create the linear, dma-buf-exported destination image + its memory, and export the
// fd. Allocates host-visible memory when available so the export is mmap-verifiable
// (the compositor imports the fd via GPU regardless). Idempotent-ish: builds once.
bool VkRenderTarget::render_and_export_linear(std::uint32_t bg_argb, int left, int top,
                                              int right, int bottom,
                                              std::uint32_t rect_argb, DmabufFrame* out,
                                              std::string* error) {
  Impl& vk_ = *impl_;
  const auto fail = [&](std::string_view msg) {
    if (error) *error = std::string(msg);
    return false;
  };
  if (!vk_.surface || !vk_.context) return fail("no render target");
  if (!vk_.dmabuf_export_ok) return fail("dma-buf export extensions unavailable");

  // ── 1. Render the scene into the optimal Skia surface ──────────────────────
  draw_scene(vk_.surface->getCanvas(), bg_argb, left, top, right, bottom, rect_argb);

  // ── 2. Build the linear exported image (once) ──────────────────────────────
  if (!vk_.export_image) {
    constexpr vk::ExternalMemoryImageCreateInfo ext_img{
        vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    vk::ImageCreateInfo ci;
    ci.imageType = vk::ImageType::e2D;
    ci.format = kVkFormat;
    ci.extent = vk::Extent3D{static_cast<std::uint32_t>(width_),
                             static_cast<std::uint32_t>(height_), 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = vk::SampleCountFlagBits::e1;
    ci.tiling = vk::ImageTiling::eLinear;
    ci.usage = vk::ImageUsageFlagBits::eTransferDst;
    ci.sharingMode = vk::SharingMode::eExclusive;
    ci.initialLayout = vk::ImageLayout::eUndefined;
    ci.setPNext(&ext_img);
    auto rv = vk_.device->createImageUnique(ci);
    if (rv.result != vk::Result::eSuccess) return fail("vkCreateImage (linear) failed");
    vk_.export_image = std::move(rv.value);

    const vk::SubresourceLayout layout = vk_.device->getImageSubresourceLayout(
        *vk_.export_image, {vk::ImageAspectFlagBits::eColor, 0, 0});

    // Memory: prefer host-visible+coherent (so we can mmap-verify), else any.
    const vk::MemoryRequirements reqs =
        vk_.device->getImageMemoryRequirements(*vk_.export_image);
    const vk::PhysicalDeviceMemoryProperties mem_props = vk_.phys_dev.getMemoryProperties();
    std::uint32_t mem_type = UINT32_MAX;
    bool host_visible = false;
    for (std::uint32_t j = 0; j < mem_props.memoryTypeCount; ++j) {
      if (!(reqs.memoryTypeBits & (1u << j))) continue;
      const vk::MemoryPropertyFlags f = mem_props.memoryTypes.at(j).propertyFlags;
      if ((f & vk::MemoryPropertyFlagBits::eHostVisible) &&
          (f & vk::MemoryPropertyFlagBits::eHostCoherent)) {
        mem_type = j;
        host_visible = true;
        break;
      }
      if (mem_type == UINT32_MAX) mem_type = j;  // fallback: first compatible
    }
    if (mem_type == UINT32_MAX) return fail("no compatible memory type");

    vk::MemoryDedicatedAllocateInfo dedicated{*vk_.export_image};
    vk::ExportMemoryAllocateInfo export_mem{
        vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    export_mem.pNext = &dedicated;
    vk::MemoryAllocateInfo mem_ai{reqs.size, mem_type};
    mem_ai.setPNext(&export_mem);
    auto mrv = vk_.device->allocateMemoryUnique(mem_ai);
    if (mrv.result != vk::Result::eSuccess) return fail("vkAllocateMemory failed");
    vk_.export_memory = std::move(mrv.value);
    if (vk_.device->bindImageMemory(*vk_.export_image, *vk_.export_memory, 0) !=
        vk::Result::eSuccess)
      return fail("vkBindImageMemory failed");

    // Export the dma-buf fd.
    auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(*vk_.device, "vkGetMemoryFdKHR"));
    if (!get_fd) return fail("vkGetMemoryFdKHR unavailable");
    const VkMemoryGetFdInfoKHR fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr,
                                       *vk_.export_memory,
                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int raw_fd = -1;
    if (static_cast<vk::Result>(get_fd(*vk_.device, &fd_info, &raw_fd)) !=
        vk::Result::eSuccess)
      return fail("vkGetMemoryFdKHR failed");
    vk_.export_fd = raw_fd;

    vk_.frame = DmabufFrame{};
    vk_.frame.fd = raw_fd;
    vk_.frame.width = static_cast<std::uint32_t>(width_);
    vk_.frame.height = static_cast<std::uint32_t>(height_);
    vk_.frame.stride = static_cast<std::uint32_t>(layout.rowPitch);
    vk_.frame.offset = static_cast<std::uint32_t>(layout.offset);
    vk_.frame.modifier = static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR);
    vk_.frame.drm_format = kDrmFormat;
    vk_.frame.mem_size = reqs.size;
    vk_.frame.host_mapped = host_visible;

    // Command buffer + fence for the copy pass.
    const vk::CommandBufferAllocateInfo cb_ai{*vk_.cmd_pool,
                                              vk::CommandBufferLevel::ePrimary, 1};
    auto crv = vk_.device->allocateCommandBuffersUnique(cb_ai);
    if (crv.result != vk::Result::eSuccess) return fail("vkAllocateCommandBuffers failed");
    vk_.copy_cmd = std::move(crv.value.front());
    auto frv = vk_.device->createFenceUnique({});
    if (frv.result != vk::Result::eSuccess) return fail("vkCreateFence failed");
    vk_.copy_fence = std::move(frv.value);
  }

  // ── 3. Flush the Skia surface into TRANSFER_SRC_OPTIMAL ────────────────────
  const skgpu::MutableTextureState transfer_src = skgpu::MutableTextureStates::MakeVulkan(
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_.queue_family);
  vk_.context->flush(vk_.surface.get(), GrFlushInfo{}, &transfer_src);
  vk_.context->submit(GrSyncCpu::kYes);

  // ── 4. Copy the optimal Skia image into the linear dma-buf image ───────────
  GrBackendTexture bt = SkSurfaces::GetBackendTexture(
      vk_.surface.get(), SkSurfaces::BackendHandleAccess::kFlushRead);
  GrVkImageInfo src_info{};
  if (!GrBackendTextures::GetVkImageInfo(bt, &src_info)) return fail("GetVkImageInfo failed");
  const vk::Image src_image{src_info.fImage};
  const auto src_layout = static_cast<vk::ImageLayout>(src_info.fImageLayout);

  const vk::CommandBuffer cmd = *vk_.copy_cmd;
  (void)cmd.reset();
  (void)cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
  const vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
  const vk::ImageMemoryBarrier to_dst{{},
                                      vk::AccessFlagBits::eTransferWrite,
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eGeneral,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *vk_.export_image,
                                      color};
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                      vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_dst);
  const vk::ImageCopy region{{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                             {0, 0, 0},
                             {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                             {0, 0, 0},
                             {static_cast<std::uint32_t>(width_),
                              static_cast<std::uint32_t>(height_), 1}};
  cmd.copyImage(src_image, src_layout, *vk_.export_image, vk::ImageLayout::eGeneral, region);
  const vk::ImageMemoryBarrier to_ext{vk::AccessFlagBits::eTransferWrite,
                                      {},
                                      vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eGeneral,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *vk_.export_image,
                                      color};
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                      vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, to_ext);
  (void)cmd.end();

  vk::SubmitInfo submit{};
  submit.setCommandBuffers(cmd);
  if (vk_.queue.submit(submit, *vk_.copy_fence) != vk::Result::eSuccess)
    return fail("vkQueueSubmit (copy) failed");
  if (vk_.device->waitForFences(*vk_.copy_fence, VK_TRUE, UINT64_MAX) !=
      vk::Result::eSuccess)
    return fail("vkWaitForFences (copy) failed");
  (void)vk_.device->resetFences(*vk_.copy_fence);

  if (out) *out = vk_.frame;
  return true;
}

bool VkRenderTarget::read_exported_pixels(Surface& out) {
  Impl& vk_ = *impl_;
  if (!vk_.export_memory || !vk_.frame.host_mapped) return false;
  if (out.width() != width_ || out.height() != height_) return false;

  auto mrv = vk_.device->mapMemory(*vk_.export_memory, 0, vk_.frame.mem_size);
  if (mrv.result != vk::Result::eSuccess) return false;
  const auto* base = static_cast<const std::uint8_t*>(mrv.value);

  std::vector<std::uint32_t> buf(static_cast<std::size_t>(width_) *
                                 static_cast<std::size_t>(height_));
  for (int y = 0; y < height_; ++y) {
    const std::uint8_t* row = base + vk_.frame.offset +
                              static_cast<std::size_t>(y) * vk_.frame.stride;
    std::memcpy(buf.data() + static_cast<std::size_t>(y) * width_, row,
                static_cast<std::size_t>(width_) * sizeof(std::uint32_t));
  }
  vk_.device->unmapMemory(*vk_.export_memory);
  out.blit(buf.data());
  return true;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_VULKAN

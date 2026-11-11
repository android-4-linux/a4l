// SPDX-License-Identifier: Apache-2.0
// Provenance: wcs example skia-vulkan-dmabuf/main.cpp (Apache-2.0/MIT) — the
// InitVulkan pattern (instance 1.1, physical-device pick, graphics queue,
// VulkanMemoryAllocators::Make, GrDirectContexts::MakeVulkan); Skia public API
// (include/gpu/ganesh/vk/GrVkDirectContext.h,
// include/gpu/vk/VulkanBackendContext.h, include/gpu/ganesh/SkSurfaceGanesh.h)
// + the VulkanMemoryAllocatorPriv Make; ADR-4 (single pinned Skia, hwui
// dropped). No GPL-3.0 sources.

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

// Use the ResultValue (.result/.value) API — no C++ exceptions — as the wcs
// example does; must precede <vulkan/vulkan.hpp>.
#define VULKAN_HPP_NO_EXCEPTIONS
// With exceptions off, vulkan-hpp still asserts on every non-success Result
// before handing it back, so the assert fires inside the call and the `.result`
// checks below never run. On a machine with no Vulkan driver that turns
// "Create() returns an error the caller reports" into SIGABRT inside
// vkCreateInstance. Every call site here checks its own Result, so the
// library's assert is not a safety net, it is a second opinion that aborts
// first.
#define VULKAN_HPP_ASSERT_ON_RESULT(expr) ((void)0)
#include <vulkan/vulkan.hpp>

#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
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
#include "include/gpu/vk/VulkanTypes.h"  // skgpu::VulkanAlloc
// Private headers: the bundled VMA factory + the ThreadSafe enum it takes (the
// same includes the wcs example uses; pinned to the m150 build, ADR-4).
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"

namespace acl::gfx {

namespace {
// gfx::Surface stores 0xAARRGGBB; little-endian byte order B,G,R,A =
// kBGRA_8888, matching kVkFormat below (the same choice the wcs example makes).
constexpr SkColorType kSkColorType = kBGRA_8888_SkColorType;
constexpr vk::Format kVkFormat = vk::Format::eB8G8R8A8Unorm;
constexpr std::uint32_t kDrmFormat =
    DRM_FORMAT_XRGB8888;  // opaque; matches kVkFormat (BGRA)
// (The video hole-punch's ARGB toplevel is on the graphics_jni GPU-present path
// — tools/aosp/ graphics_jni/gpu_dmabuf.cpp — which is what real apps use; this
// acl-native path stays opaque.)
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
  bool modifier_ext_ok = false;  // VK_EXT_image_drm_format_modifier available
  vk::UniqueCommandPool cmd_pool;
  vk::UniqueImage export_image;
  vk::UniqueDeviceMemory export_memory;
  vk::UniqueCommandBuffer copy_cmd;
  vk::UniqueFence copy_fence;
  int export_fd = -1;

  // Modifier-tiled direct path (V2b): the exported tiled image Skia renders
  // into.
  vk::UniqueImage mod_image;
  vk::UniqueDeviceMemory mod_memory;
  sk_sp<SkSurface> mod_surface;
  int mod_fd = -1;

  SkSurface* last_rendered = nullptr;  // for read_rendered_pixels
  DmabufFrame frame{};                 // last exported descriptor
};

VkRenderTarget::VkRenderTarget() : impl_(std::make_unique<Impl>()) {}
VkRenderTarget::~VkRenderTarget() {
  if (impl_) {
    if (impl_->context)
      impl_->context->flushAndSubmit(GrSyncCpu::kYes);
    if (impl_->export_fd >= 0)
      ::close(impl_->export_fd);
    if (impl_->mod_fd >= 0)
      ::close(impl_->mod_fd);
  }
}

bool VkRenderTarget::dmabuf_export_available() const {
  return impl_ && impl_->dmabuf_export_ok;
}

bool VkRenderTarget::modifier_tiled_available() const {
  return impl_ && impl_->modifier_ext_ok;
}

std::unique_ptr<VkRenderTarget> VkRenderTarget::Create(int width,
                                                       int height,
                                                       std::string* error) {
  const auto fail =
      [&](std::string_view msg) -> std::unique_ptr<VkRenderTarget> {
    if (error)
      *error = std::string(msg);
    return nullptr;
  };
  if (width <= 0 || height <= 0)
    return fail("non-positive size");

  auto rt = std::unique_ptr<VkRenderTarget>(new VkRenderTarget());
  rt->width_ = width;
  rt->height_ = height;
  Impl& vk_ = *rt->impl_;

  // ── Instance (Vulkan 1.1 — Skia's minimum) ─────────────────────────────────
  constexpr vk::ApplicationInfo app_info{"acl-gfx", VK_MAKE_VERSION(0, 1, 0),
                                         nullptr, 0, VK_API_VERSION_1_1};
  const vk::InstanceCreateInfo inst_ci{{}, &app_info};
  auto inst_rv = vk::createInstanceUnique(inst_ci);
  if (inst_rv.result != vk::Result::eSuccess)
    return fail("vkCreateInstance failed");
  vk_.instance = std::move(inst_rv.value);

  // ── Physical device (prefer a real GPU over a CPU/llvmpipe renderer)
  // ────────
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
  if (vk_.queue_family == UINT32_MAX)
    return fail("no graphics queue family");

  // ── Device extensions: dma-buf export (V2). Their absence just disables the
  //     dma-buf path (V1 offscreen render + readback still works).
  //     ─────────────
  auto ext_rv = vk_.phys_dev.enumerateDeviceExtensionProperties();
  if (ext_rv.result == vk::Result::eSuccess) {
    const auto has_ext = [&](std::string_view name) {
      return std::any_of(ext_rv.value.begin(), ext_rv.value.end(),
                         [&](const vk::ExtensionProperties& e) {
                           return std::string_view{e.extensionName.data()} ==
                                  name;
                         });
    };
    if (has_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
        has_ext(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
      vk_.device_exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
      vk_.device_exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
      vk_.dmabuf_export_ok = true;
      // Modifier-tiled direct path (zero-copy) needs these; their absence
      // forces the linear copy-fallback.
      if (has_ext(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
          has_ext(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) {
        vk_.device_exts.push_back(
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        vk_.device_exts.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        vk_.modifier_ext_ok = true;
      }
    }
  }

  // ── Logical device ─────────────────────────────────────────────────────────
  constexpr float queue_prio = 1.0f;
  const vk::DeviceQueueCreateInfo queue_ci{
      {}, vk_.queue_family, 1, &queue_prio};
  const vk::DeviceCreateInfo dev_ci{
      {},
      1,
      &queue_ci,
      0,
      nullptr,
      static_cast<std::uint32_t>(vk_.device_exts.size()),
      vk_.device_exts.data()};
  auto dev_rv = vk_.phys_dev.createDeviceUnique(dev_ci);
  if (dev_rv.result != vk::Result::eSuccess)
    return fail("vkCreateDevice failed");
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
    if (device != VK_NULL_HANDLE)
      return vkGetDeviceProcAddr(device, name);
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
  vk_.allocator =
      skgpu::VulkanMemoryAllocators::Make(backend, skgpu::ThreadSafe::kNo);
  if (!vk_.allocator)
    return fail("VulkanMemoryAllocators::Make failed");
  backend.fMemoryAllocator = vk_.allocator;

  vk_.context = GrDirectContexts::MakeVulkan(backend);
  if (!vk_.context)
    return fail("GrDirectContexts::MakeVulkan failed");

  // ── Offscreen GPU render target
  // ───────────────────────────────────────────── GPU render targets require
  // premultiplied alpha (a Ganesh constraint, and what the wcs example uses);
  // readback below converts to the gfx unpremul convention. kTopLeft origin so
  // the VkImage memory is stored top-down: RenderTarget defaults to
  // kBottomLeft, which readPixels hides but a raw vkCmdCopyImage into the
  // dma-buf (V2) would copy verbatim — i.e. vertically flipped. Wayland
  // surfaces are top-left origin too, so top-down is the correct storage for
  // both readback and present.
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kSkColorType, kPremul_SkAlphaType);
  vk_.surface =
      SkSurfaces::RenderTarget(vk_.context.get(), skgpu::Budgeted::kYes, info,
                               /*sampleCount=*/0, kTopLeft_GrSurfaceOrigin,
                               /*surfaceProps=*/nullptr);
  if (!vk_.surface)
    return fail("SkSurfaces::RenderTarget (Vulkan) failed");

  return rt;
}

namespace {
// The V2 scene stand-in (mirrors gfx_skia.cpp / render_rect): a background
// clear plus one AA-off filled rect, so GPU output is bit-comparable to the
// software path.
void draw_scene(SkCanvas* canvas,
                std::uint32_t bg_argb,
                int left,
                int top,
                int right,
                int bottom,
                std::uint32_t rect_argb) {
  canvas->clear(static_cast<SkColor>(bg_argb));
  SkPaint paint;
  paint.setColor(static_cast<SkColor>(rect_argb));
  paint.setAntiAlias(false);
  canvas->drawRect(
      SkRect::MakeLTRB(static_cast<SkScalar>(left), static_cast<SkScalar>(top),
                       static_cast<SkScalar>(right),
                       static_cast<SkScalar>(bottom)),
      paint);
}
}  // namespace

bool VkRenderTarget::render_rect(Surface& out,
                                 std::uint32_t bg_argb,
                                 int left,
                                 int top,
                                 int right,
                                 int bottom,
                                 std::uint32_t rect_argb) {
  Impl& vk_ = *impl_;
  if (!vk_.surface || !vk_.context)
    return false;
  if (out.width() != width_ || out.height() != height_)
    return false;

  draw_scene(vk_.surface->getCanvas(), bg_argb, left, top, right, bottom,
             rect_argb);

  // Flush GPU work and block until it completes, then read the pixels back.
  vk_.context->flushAndSubmit(GrSyncCpu::kYes);

  const SkImageInfo info =
      SkImageInfo::Make(width_, height_, kSkColorType, kUnpremul_SkAlphaType);
  std::vector<std::uint32_t> buf(static_cast<std::size_t>(width_) *
                                 static_cast<std::size_t>(height_));
  if (!vk_.surface->readPixels(
          info, buf.data(),
          static_cast<std::size_t>(width_) * sizeof(std::uint32_t), 0, 0)) {
    return false;
  }
  out.blit(buf.data());
  return true;
}

// Create the linear, dma-buf-exported destination image + its memory (once),
// and export the fd. Allocates host-visible memory when available so the export
// is mmap-verifiable AND writable (S3b: acl uploads android.graphics CPU pixels
// into it). The compositor imports the fd via GPU regardless. Shared by
// render_and_export_linear (GPU copy in) and import_pixels_to_dmabuf (CPU write
// in).
bool VkRenderTarget::ensure_linear_export(std::string* error) {
  Impl& vk_ = *impl_;
  const auto fail = [&](std::string_view msg) {
    if (error)
      *error = std::string(msg);
    return false;
  };
  if (vk_.export_image)
    return true;
  if (!vk_.dmabuf_export_ok)
    return fail("dma-buf export extensions unavailable");

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
  if (rv.result != vk::Result::eSuccess)
    return fail("vkCreateImage (linear) failed");
  vk_.export_image = std::move(rv.value);

  const vk::SubresourceLayout layout = vk_.device->getImageSubresourceLayout(
      *vk_.export_image, {vk::ImageAspectFlagBits::eColor, 0, 0});

  // Memory: prefer host-visible+coherent (so we can mmap read/write), else any.
  const vk::MemoryRequirements reqs =
      vk_.device->getImageMemoryRequirements(*vk_.export_image);
  const vk::PhysicalDeviceMemoryProperties mem_props =
      vk_.phys_dev.getMemoryProperties();
  std::uint32_t mem_type = UINT32_MAX;
  bool host_visible = false;
  for (std::uint32_t j = 0; j < mem_props.memoryTypeCount; ++j) {
    if (!(reqs.memoryTypeBits & (1u << j)))
      continue;
    const vk::MemoryPropertyFlags f = mem_props.memoryTypes.at(j).propertyFlags;
    if ((f & vk::MemoryPropertyFlagBits::eHostVisible) &&
        (f & vk::MemoryPropertyFlagBits::eHostCoherent)) {
      mem_type = j;
      host_visible = true;
      break;
    }
    if (mem_type == UINT32_MAX)
      mem_type = j;  // fallback: first compatible
  }
  if (mem_type == UINT32_MAX)
    return fail("no compatible memory type");

  vk::MemoryDedicatedAllocateInfo dedicated{*vk_.export_image};
  vk::ExportMemoryAllocateInfo export_mem{
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  export_mem.pNext = &dedicated;
  vk::MemoryAllocateInfo mem_ai{reqs.size, mem_type};
  mem_ai.setPNext(&export_mem);
  auto mrv = vk_.device->allocateMemoryUnique(mem_ai);
  if (mrv.result != vk::Result::eSuccess)
    return fail("vkAllocateMemory failed");
  vk_.export_memory = std::move(mrv.value);
  if (vk_.device->bindImageMemory(*vk_.export_image, *vk_.export_memory, 0) !=
      vk::Result::eSuccess)
    return fail("vkBindImageMemory failed");

  auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
      vkGetDeviceProcAddr(*vk_.device, "vkGetMemoryFdKHR"));
  if (!get_fd)
    return fail("vkGetMemoryFdKHR unavailable");
  const VkMemoryGetFdInfoKHR fd_info{
      VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, *vk_.export_memory,
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
  vk_.frame.plane_count = 1;
  vk_.frame.planes[0] = {static_cast<std::uint32_t>(layout.offset),
                         static_cast<std::uint32_t>(layout.rowPitch)};
  vk_.frame.modifier = static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR);
  vk_.frame.drm_format = kDrmFormat;
  vk_.frame.mem_size = reqs.size;
  vk_.frame.host_mapped = host_visible;
  vk_.frame.modifier_tiled = false;

  const vk::CommandBufferAllocateInfo cb_ai{
      *vk_.cmd_pool, vk::CommandBufferLevel::ePrimary, 1};
  auto crv = vk_.device->allocateCommandBuffersUnique(cb_ai);
  if (crv.result != vk::Result::eSuccess)
    return fail("vkAllocateCommandBuffers failed");
  vk_.copy_cmd = std::move(crv.value.front());
  auto frv = vk_.device->createFenceUnique({});
  if (frv.result != vk::Result::eSuccess)
    return fail("vkCreateFence failed");
  vk_.copy_fence = std::move(frv.value);
  return true;
}

// S3b: import CPU pixels (a rendered android.graphics Bitmap → gfx::Surface)
// into the linear dma-buf by writing its host-visible memory, and hand back the
// frame to present. The GPU-native zero-copy path (hwui/Ganesh rendering
// straight into the dma-buf) is S4; this closes the loop today. Requires
// host-visible export memory (frame.host_mapped).
bool VkRenderTarget::import_pixels_to_dmabuf(const Surface& src,
                                             DmabufFrame* out,
                                             std::string* error) {
  Impl& vk_ = *impl_;
  const auto fail = [&](std::string_view msg) {
    if (error)
      *error = std::string(msg);
    return false;
  };
  if (src.width() != width_ || src.height() != height_)
    return fail("size mismatch");
  if (!ensure_linear_export(error))
    return false;
  if (!vk_.frame.host_mapped)
    return fail("export memory not host-visible");

  auto mrv = vk_.device->mapMemory(*vk_.export_memory, 0, vk_.frame.mem_size);
  if (mrv.result != vk::Result::eSuccess)
    return fail("vkMapMemory failed");
  auto* base = static_cast<std::uint8_t*>(mrv.value);
  const std::uint32_t* pix = src.data();
  for (int y = 0; y < height_; ++y) {
    std::memcpy(base + vk_.frame.offset() +
                    static_cast<std::size_t>(y) * vk_.frame.stride(),
                pix + static_cast<std::size_t>(y) * width_,
                static_cast<std::size_t>(width_) * sizeof(std::uint32_t));
  }
  vk_.device->unmapMemory(*vk_.export_memory);
  if (out)
    *out = vk_.frame;
  return true;
}

// Create the linear, dma-buf-exported destination image + its memory, and
// export the fd. Allocates host-visible memory when available so the export is
// mmap-verifiable (the compositor imports the fd via GPU regardless).
// Idempotent-ish: builds once.
bool VkRenderTarget::render_and_export_linear(std::uint32_t bg_argb,
                                              int left,
                                              int top,
                                              int right,
                                              int bottom,
                                              std::uint32_t rect_argb,
                                              DmabufFrame* out,
                                              std::string* error) {
  Impl& vk_ = *impl_;
  const auto fail = [&](std::string_view msg) {
    if (error)
      *error = std::string(msg);
    return false;
  };
  if (!vk_.surface || !vk_.context)
    return fail("no render target");
  if (!vk_.dmabuf_export_ok)
    return fail("dma-buf export extensions unavailable");

  // ── 1. Render the scene into the optimal Skia surface ──────────────────────
  draw_scene(vk_.surface->getCanvas(), bg_argb, left, top, right, bottom,
             rect_argb);
  vk_.last_rendered = vk_.surface.get();

  // ── 2. Build the linear exported image (once) ──────────────────────────────
  if (!ensure_linear_export(error))
    return false;

  // ── 3. Flush the Skia surface into TRANSFER_SRC_OPTIMAL ────────────────────
  const skgpu::MutableTextureState transfer_src =
      skgpu::MutableTextureStates::MakeVulkan(
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_.queue_family);
  vk_.context->flush(vk_.surface.get(), GrFlushInfo{}, &transfer_src);
  vk_.context->submit(GrSyncCpu::kYes);

  // ── 4. Copy the optimal Skia image into the linear dma-buf image ───────────
  GrBackendTexture bt = SkSurfaces::GetBackendTexture(
      vk_.surface.get(), SkSurfaces::BackendHandleAccess::kFlushRead);
  GrVkImageInfo src_info{};
  if (!GrBackendTextures::GetVkImageInfo(bt, &src_info))
    return fail("GetVkImageInfo failed");
  const vk::Image src_image{src_info.fImage};
  const auto src_layout = static_cast<vk::ImageLayout>(src_info.fImageLayout);

  const vk::CommandBuffer cmd = *vk_.copy_cmd;
  (void)cmd.reset();
  (void)cmd.begin(vk::CommandBufferBeginInfo{
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
  const vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1,
                                        0, 1};
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
  cmd.copyImage(src_image, src_layout, *vk_.export_image,
                vk::ImageLayout::eGeneral, region);
  const vk::ImageMemoryBarrier to_ext{vk::AccessFlagBits::eTransferWrite,
                                      {},
                                      vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eGeneral,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *vk_.export_image,
                                      color};
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                      vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {},
                      to_ext);
  (void)cmd.end();

  vk::SubmitInfo submit{};
  submit.setCommandBuffers(cmd);
  if (vk_.queue.submit(submit, *vk_.copy_fence) != vk::Result::eSuccess)
    return fail("vkQueueSubmit (copy) failed");
  if (vk_.device->waitForFences(*vk_.copy_fence, VK_TRUE, UINT64_MAX) !=
      vk::Result::eSuccess)
    return fail("vkWaitForFences (copy) failed");
  (void)vk_.device->resetFences(*vk_.copy_fence);

  if (out)
    *out = vk_.frame;
  return true;
}

bool VkRenderTarget::read_exported_pixels(Surface& out) {
  Impl& vk_ = *impl_;
  if (!vk_.export_memory || !vk_.frame.host_mapped)
    return false;
  if (out.width() != width_ || out.height() != height_)
    return false;

  auto mrv = vk_.device->mapMemory(*vk_.export_memory, 0, vk_.frame.mem_size);
  if (mrv.result != vk::Result::eSuccess)
    return false;
  const auto* base = static_cast<const std::uint8_t*>(mrv.value);

  std::vector<std::uint32_t> buf(static_cast<std::size_t>(width_) *
                                 static_cast<std::size_t>(height_));
  for (int y = 0; y < height_; ++y) {
    const std::uint8_t* row = base + vk_.frame.offset() +
                              static_cast<std::size_t>(y) * vk_.frame.stride();
    std::memcpy(buf.data() + static_cast<std::size_t>(y) * width_, row,
                static_cast<std::size_t>(width_) * sizeof(std::uint32_t));
  }
  vk_.device->unmapMemory(*vk_.export_memory);
  out.blit(buf.data());
  return true;
}

std::vector<std::uint64_t> VkRenderTarget::renderable_modifiers(
    const std::vector<std::uint64_t>& candidate_modifiers) const {
  std::vector<std::uint64_t> out;
  Impl& vk_ = *impl_;
  if (!vk_.modifier_ext_ok)
    return out;

  // Query the GPU's DRM-format-modifier support for kVkFormat (two-call idiom).
  vk::DrmFormatModifierPropertiesListEXT list{};
  vk::FormatProperties2 fp2{};
  fp2.pNext = &list;
  vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);
  std::vector<vk::DrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);

  constexpr auto need = vk::FormatFeatureFlagBits::eColorAttachment |
                        vk::FormatFeatureFlagBits::eSampledImage;
  for (std::uint64_t m : candidate_modifiers) {
    if (m == DRM_FORMAT_MOD_INVALID)
      continue;
    const bool ok =
        std::any_of(mods.begin(), mods.end(),
                    [&](const vk::DrmFormatModifierPropertiesEXT& p) {
                      return p.drmFormatModifier == m &&
                             (p.drmFormatModifierTilingFeatures & need) == need;
                    });
    if (ok)
      out.push_back(m);
  }
  return out;
}

std::vector<std::uint64_t> VkRenderTarget::all_renderable_modifiers() const {
  Impl& vk_ = *impl_;
  std::vector<std::uint64_t> out;
  if (!vk_.modifier_ext_ok)
    return out;
  vk::DrmFormatModifierPropertiesListEXT list{};
  vk::FormatProperties2 fp2{};
  fp2.pNext = &list;
  vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);
  std::vector<vk::DrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);
  constexpr auto need = vk::FormatFeatureFlagBits::eColorAttachment |
                        vk::FormatFeatureFlagBits::eSampledImage;
  std::uint64_t linear = 0;
  bool has_linear = false;
  for (const auto& p : mods) {
    if ((p.drmFormatModifierTilingFeatures & need) != need)
      continue;
    if (p.drmFormatModifier ==
        static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR)) {
      has_linear = true;
      linear = p.drmFormatModifier;
    } else {
      out.push_back(p.drmFormatModifier);  // tiled first
    }
  }
  if (has_linear)
    out.push_back(linear);  // LINEAR last
  return out;
}

bool VkRenderTarget::render_to_modifier_dmabuf(
    const std::vector<std::uint64_t>& candidate_modifiers,
    std::uint32_t bg_argb,
    int left,
    int top,
    int right,
    int bottom,
    std::uint32_t rect_argb,
    DmabufFrame* out,
    std::string* error) {
  Impl& vk_ = *impl_;
  const auto fail = [&](std::string_view msg) {
    if (error)
      *error = std::string(msg);
    return false;
  };
  if (!vk_.context)
    return fail("no context");
  if (!vk_.dmabuf_export_ok)
    return fail("dma-buf export unavailable");
  if (!vk_.modifier_ext_ok)
    return fail("modifier extensions unavailable");

  // Build the modifier-tiled exported image once (a fixed-size target).
  if (!vk_.mod_surface) {
    // Pick the modifier's plane count from the GPU's properties for the chosen
    // mod.
    const std::vector<std::uint64_t> ok =
        renderable_modifiers(candidate_modifiers);
    if (ok.empty())
      return fail("no candidate modifier is GPU render+sampleable");

    vk::DrmFormatModifierPropertiesListEXT list{};
    vk::FormatProperties2 fp2{};
    fp2.pNext = &list;
    vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);
    std::vector<vk::DrmFormatModifierPropertiesEXT> props(
        list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = props.data();
    vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);

    const std::uint64_t chosen = ok.front();
    std::uint32_t plane_count = 1;
    for (const auto& p : props)
      if (p.drmFormatModifier == chosen)
        plane_count = p.drmFormatModifierPlaneCount;
    plane_count = std::min<std::uint32_t>(plane_count, DmabufFrame::kMaxPlanes);

    // Modifier-tiled, dma-buf-exported, color-attachment+sampled image.
    const std::array<std::uint64_t, 1> mods{chosen};
    vk::ImageDrmFormatModifierListCreateInfoEXT mod_ci{};
    mod_ci.setDrmFormatModifiers(mods);
    vk::ExternalMemoryImageCreateInfo ext_img{
        vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    ext_img.pNext = &mod_ci;
    vk::ImageCreateInfo ci;
    ci.imageType = vk::ImageType::e2D;
    ci.format = kVkFormat;
    ci.extent = vk::Extent3D{static_cast<std::uint32_t>(width_),
                             static_cast<std::uint32_t>(height_), 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = vk::SampleCountFlagBits::e1;
    ci.tiling = vk::ImageTiling::eDrmFormatModifierEXT;
    ci.usage = vk::ImageUsageFlagBits::eColorAttachment |
               vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferSrc |
               vk::ImageUsageFlagBits::eTransferDst;
    ci.sharingMode = vk::SharingMode::eExclusive;
    ci.initialLayout = vk::ImageLayout::eUndefined;
    ci.setPNext(&ext_img);
    {
      auto rv = vk_.device->createImageUnique(ci);
      if (rv.result != vk::Result::eSuccess)
        return fail("vkCreateImage (modifier) failed");
      vk_.mod_image = std::move(rv.value);
    }

    // Per-plane offset/stride (a modifier may span planes, e.g. AMD DCC
    // metadata).
    static constexpr std::array<vk::ImageAspectFlagBits, 4> kPlaneAspect = {
        vk::ImageAspectFlagBits::eMemoryPlane0EXT,
        vk::ImageAspectFlagBits::eMemoryPlane1EXT,
        vk::ImageAspectFlagBits::eMemoryPlane2EXT,
        vk::ImageAspectFlagBits::eMemoryPlane3EXT};
    DmabufFrame f{};
    f.plane_count = plane_count;
    for (std::uint32_t i = 0; i < plane_count; ++i) {
      const vk::SubresourceLayout l = vk_.device->getImageSubresourceLayout(
          *vk_.mod_image, {kPlaneAspect.at(i), 0, 0});
      f.planes[i] = {static_cast<std::uint32_t>(l.offset),
                     static_cast<std::uint32_t>(l.rowPitch)};
    }

    // Dedicated exportable device-local memory + fd.
    const vk::MemoryRequirements reqs =
        vk_.device->getImageMemoryRequirements(*vk_.mod_image);
    const vk::PhysicalDeviceMemoryProperties mp =
        vk_.phys_dev.getMemoryProperties();
    std::uint32_t mt = UINT32_MAX;
    for (std::uint32_t j = 0; j < mp.memoryTypeCount; ++j) {
      if (!(reqs.memoryTypeBits & (1u << j)))
        continue;
      if (mp.memoryTypes.at(j).propertyFlags &
          vk::MemoryPropertyFlagBits::eDeviceLocal) {
        mt = j;
        break;
      }
      if (mt == UINT32_MAX)
        mt = j;
    }
    if (mt == UINT32_MAX)
      return fail("no compatible memory type (modifier)");
    vk::MemoryDedicatedAllocateInfo dedicated{*vk_.mod_image};
    vk::ExportMemoryAllocateInfo export_mem{
        vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
    export_mem.pNext = &dedicated;
    vk::MemoryAllocateInfo mem_ai{reqs.size, mt};
    mem_ai.setPNext(&export_mem);
    {
      auto rv = vk_.device->allocateMemoryUnique(mem_ai);
      if (rv.result != vk::Result::eSuccess)
        return fail("vkAllocateMemory (modifier) failed");
      vk_.mod_memory = std::move(rv.value);
    }
    if (vk_.device->bindImageMemory(*vk_.mod_image, *vk_.mod_memory, 0) !=
        vk::Result::eSuccess)
      return fail("vkBindImageMemory (modifier) failed");
    auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(*vk_.device, "vkGetMemoryFdKHR"));
    if (!get_fd)
      return fail("vkGetMemoryFdKHR unavailable");
    const VkMemoryGetFdInfoKHR fd_info{
        VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, *vk_.mod_memory,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int raw_fd = -1;
    if (static_cast<vk::Result>(get_fd(*vk_.device, &fd_info, &raw_fd)) !=
        vk::Result::eSuccess)
      return fail("vkGetMemoryFdKHR (modifier) failed");
    vk_.mod_fd = raw_fd;

    f.fd = raw_fd;
    f.width = static_cast<std::uint32_t>(width_);
    f.height = static_cast<std::uint32_t>(height_);
    f.modifier = chosen;
    f.drm_format = kDrmFormat;
    f.mem_size = reqs.size;
    f.host_mapped = false;
    f.modifier_tiled = true;
    vk_.frame = f;

    // Wrap the tiled image as a Ganesh SkSurface — Skia renders straight in.
    skgpu::VulkanAlloc alloc;
    alloc.fMemory = *vk_.mod_memory;
    alloc.fOffset = 0;
    alloc.fSize = reqs.size;
    GrVkImageInfo info;
    info.fImage = *vk_.mod_image;
    info.fAlloc = alloc;
    info.fImageTiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.fFormat = static_cast<VkFormat>(kVkFormat);
    info.fImageUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.fSampleCount = 1;
    info.fLevelCount = 1;
    info.fCurrentQueueFamily = vk_.queue_family;
    info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    GrBackendTexture bt = GrBackendTextures::MakeVk(width_, height_, info);
    vk_.mod_surface = SkSurfaces::WrapBackendTexture(
        vk_.context.get(), bt, kTopLeft_GrSurfaceOrigin, 1, kSkColorType,
        nullptr, nullptr);
    if (!vk_.mod_surface)
      return fail("WrapBackendTexture failed");
  }

  // Render straight into the tiled surface — no copy — and leave it
  // compositor-readable.
  draw_scene(vk_.mod_surface->getCanvas(), bg_argb, left, top, right, bottom,
             rect_argb);
  vk_.last_rendered = vk_.mod_surface.get();
  const skgpu::MutableTextureState to_present =
      skgpu::MutableTextureStates::MakeVulkan(VK_IMAGE_LAYOUT_GENERAL,
                                              vk_.queue_family);
  vk_.context->flush(vk_.mod_surface.get(), GrFlushInfo{}, &to_present);
  vk_.context->submit(GrSyncCpu::kYes);

  if (out)
    *out = vk_.frame;
  return true;
}

bool VkRenderTarget::read_rendered_pixels(Surface& out) {
  Impl& vk_ = *impl_;
  if (!vk_.last_rendered)
    return false;
  if (out.width() != width_ || out.height() != height_)
    return false;
  const SkImageInfo info =
      SkImageInfo::Make(width_, height_, kSkColorType, kUnpremul_SkAlphaType);
  std::vector<std::uint32_t> buf(static_cast<std::size_t>(width_) *
                                 static_cast<std::size_t>(height_));
  if (!vk_.last_rendered->readPixels(
          info, buf.data(),
          static_cast<std::size_t>(width_) * sizeof(std::uint32_t), 0, 0))
    return false;
  out.blit(buf.data());
  return true;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_VULKAN

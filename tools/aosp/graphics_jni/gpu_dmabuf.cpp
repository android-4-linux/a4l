// SPDX-License-Identifier: Apache-2.0
// Provenance: acl P2.4/S4a; ports acl gfx_vk render_to_modifier_dmabuf (Apache-2.0,
// vulkan-hpp); Vulkan-Hpp dynamic-dispatcher pattern from ivi-homescreen
// vulkan_drm_backend.cc (Apache-2.0); Skia public Ganesh Vulkan API. No GPL-3.0 sources.
//
// GPU-native render (S4a step 2): render the scene with Android's (now Vulkan-enabled)
// host Skia Ganesh into a Skia-owned OPTIMAL surface, then vkCmdCopyImage it into a
// modifier-tiled, dma-buf-exported VkImage (GPU->GPU, no CPU upload). acl presents the
// exported fd via zwp_linux_dmabuf_v1 (V2b).
//
// Loader: Vulkan-Hpp's DEFAULT dynamic dispatcher — init() [internal DynamicLoader],
// then init(instance), then init(device) — so EVERY Vulkan call (ours AND Skia's, via a
// GrVkGetProc sourced from the same dispatcher) uses ONE consistent dispatch. A separate
// dlopen + ad-hoc vkGetDeviceProcAddr gave an inconsistent device dispatch under ART
// ("VkDevice dispatch handle not found") that silently broke Skia's render targets.

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_EXCEPTIONS  // Soong builds -fno-exceptions
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include <drm_fourcc.h>

#include "GrBackendSurface.h"
#include "GrBackendSurfaceMutableState.h"
#include "GrDirectContext.h"
#include "SkCanvas.h"
#include "SkColor.h"
#include "SkColorSpace.h"
#include "SkImageInfo.h"
#include "SkPaint.h"
#include "SkRect.h"
#include "SkSurface.h"
#include "gpu/vk/GrVkBackendContext.h"
#include "gpu/vk/GrVkExtensions.h"
#include "gpu/vk/GrVkTypes.h"

extern "C" struct AclDmabuf {
  int fd;
  uint32_t width, height, plane_count;
  uint32_t offsets[4], strides[4];
  uint64_t modifier;
  uint32_t drm_format;
};

namespace {

#define VKD VULKAN_HPP_DEFAULT_DISPATCHER  // the one dispatcher everything uses

constexpr VkFormat kVkFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kDrmFormat = DRM_FORMAT_XRGB8888;

// One exported, modifier-tiled dma-buf VkImage + its memory/fd/descriptor. The animated
// present loop ping-pongs two of these so the compositor can hold one for scanout while
// Skia renders the next — see acl_graphics_gpu_end_frame(buffer_index, ...).
struct Target {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize mem_size = 0;
  int fd = -1;
  AclDmabuf info{};
  bool allocated = false;
};

// 3 ping-pong dma-buf targets: the deferred RenderThread pipeline needs one displayed by the
// compositor, one being rendered, and one in transit — 2 starves (buffer collisions -> flicker).
constexpr int kBuffers = 3;

struct Gpu {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t qfam = UINT32_MAX;
  // Enabled device extension list + features2 chain, kept for the WebView AwDrawFn InitVk (hands
  // Chromium the host device it must render Skia into — see the VK functor path).
  const char* const* device_exts = nullptr;
  uint32_t device_ext_count = 0;
  VkPhysicalDeviceFeatures2* features2 = nullptr;
  VkImageUsageFlags usage = 0;
  Target targets[kBuffers];  // ping-pong dma-buf images (copy destinations)
  sk_sp<GrDirectContext> gr;
  sk_sp<SkSurface> opt;      // single Skia-owned optimal render target (copy-path src)
  sk_sp<SkSurface> direct;   // a dma-buf VkImage wrapped as SkSurface (zero-copy direct path)
  GrVkExtensions ext;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  int w = 0, h = 0;
  bool ready = false;
} g;

bool init_device() {
  // This vulkan-headers has no no-arg init(); feed it the loader's vkGetInstanceProcAddr,
  // then re-init at instance and device scope so ALL dispatch is consistent.
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) { std::fprintf(stderr, "acl-gpu: no libvulkan\n"); return false; }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
  if (gipa == nullptr) { std::fprintf(stderr, "acl-gpu: no vkGetInstanceProcAddr\n"); return false; }
  VKD.init(gipa);  // loads global entrypoints (vkCreateInstance, ...) via gipa(nullptr, name)
  if (VKD.vkCreateInstance == nullptr) { std::fprintf(stderr, "acl-gpu: no vulkan loader\n"); return false; }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (VKD.vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: createInstance fail\n");
    return false;
  }
  VKD.init(vk::Instance(g.instance));  // load ALL instance-level entrypoints

  uint32_t n = 0;
  VKD.vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
  if (n == 0) return false;
  std::vector<VkPhysicalDevice> devs(n);
  VKD.vkEnumeratePhysicalDevices(g.instance, &n, devs.data());
  g.phys = devs[0];
  for (auto d : devs) {
    VkPhysicalDeviceProperties p;
    VKD.vkGetPhysicalDeviceProperties(d, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
        p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) { g.phys = d; break; }
  }
  uint32_t qn = 0;
  VKD.vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qn);
  VKD.vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qfs.data());
  for (uint32_t i = 0; i < qn; ++i)
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.qfam = i; break; }
  if (g.qfam == UINT32_MAX) return false;

  // Kept STATIC: the Chromium WebView AwDrawFn Vulkan functor's InitVk takes the host's device +
  // enabled extension list + features2 and builds Skia's GrVkBackendContext from them (which may keep
  // the pointers), so this list and the features2 chain must outlive device creation. The added
  // VK_KHR_external_semaphore_fd + the ycbcr-conversion feature mirror what hwui's VulkanManager
  // enables and what Chromium's per-frame semaphore handshake / AHardwareBuffer sampling need.
  static const char* const kDeviceExts[] = {
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
  };
  static VkPhysicalDeviceSamplerYcbcrConversionFeatures kYcbcr{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, nullptr, VK_TRUE};
  static VkPhysicalDeviceFeatures2 kFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &kYcbcr, {}};
  g.device_exts = kDeviceExts;
  g.device_ext_count = static_cast<uint32_t>(std::size(kDeviceExts));
  g.features2 = &kFeatures2;

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = g.qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &kFeatures2;  // features2 (ycbcr) in the pNext chain; pEnabledFeatures must stay null
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = g.device_ext_count;
  dci.ppEnabledExtensionNames = kDeviceExts;
  if (VKD.vkCreateDevice(g.phys, &dci, nullptr, &g.device) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: createDevice fail\n");
    return false;
  }
  VKD.init(vk::Device(g.device));  // load ALL device-level entrypoints (consistent dispatch)
  VKD.vkGetDeviceQueue(g.device, g.qfam, 0, &g.queue);
  return true;
}

// Hand the present VkDevice + enabled extensions/features to the WebView AwDrawFn InitVk builder
// (webview_functor.cpp). Chromium renders its Skia into a secondary command buffer on THIS device, so
// its device must be the same one acl composites the frame with. Returns false until the device is up.
extern "C" bool acl_gpu_vk_handles(void** inst, void** phys, void** dev, void** q, uint32_t* qfam,
                                   const char* const** exts, uint32_t* n_exts, void** features2) {
  if (g.device == VK_NULL_HANDLE) return false;
  *inst = g.instance;
  *phys = g.phys;
  *dev = g.device;
  *q = g.queue;
  *qfam = g.qfam;
  *exts = g.device_exts;
  *n_exts = g.device_ext_count;
  *features2 = g.features2;
  return true;
}

// Prefer a single-plane tiled modifier the GPU can render+sample; fall back to LINEAR.
uint64_t choose_modifier(uint32_t* plane_count) {
  VkDrmFormatModifierPropertiesListEXT list{
      VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 fp2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
  VKD.vkGetPhysicalDeviceFormatProperties2(g.phys, kVkFormat, &fp2);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  VKD.vkGetPhysicalDeviceFormatProperties2(g.phys, kVkFormat, &fp2);
  constexpr VkFormatFeatureFlags need =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  uint64_t tiled1 = DRM_FORMAT_MOD_INVALID, linear = DRM_FORMAT_MOD_INVALID;
  for (const auto& m : mods) {
    if ((m.drmFormatModifierTilingFeatures & need) != need) continue;
    if (m.drmFormatModifierPlaneCount != 1) continue;
    if (m.drmFormatModifier == DRM_FORMAT_MOD_LINEAR) { linear = m.drmFormatModifier; continue; }
    if (tiled1 == DRM_FORMAT_MOD_INVALID) tiled1 = m.drmFormatModifier;
  }
  if (tiled1 != DRM_FORMAT_MOD_INVALID) { *plane_count = 1; return tiled1; }
  if (linear != DRM_FORMAT_MOD_INVALID) { *plane_count = 1; return linear; }
  return DRM_FORMAT_MOD_INVALID;
}

// Create one modifier-tiled, dma-buf-exported VkImage + memory + fd into `t` (a plain
// VkImage, the vkCmdCopyImage DST — the scene is rendered into g.opt and copied here).
bool make_target(Target& t, int w, int h) {
  uint32_t plane_count = 1;
  const uint64_t modifier = choose_modifier(&plane_count);
  if (modifier == DRM_FORMAT_MOD_INVALID) { std::fprintf(stderr, "acl-gpu: no modifier\n"); return false; }
  AclDmabuf* out = &t.info;

  const uint64_t mods[1] = {modifier};
  VkImageDrmFormatModifierListCreateInfoEXT modci{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
  modci.drmFormatModifierCount = 1;
  modci.pDrmFormatModifiers = mods;
  VkExternalMemoryImageCreateInfo extci{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modci,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extci};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = kVkFormat;
  ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  g.usage = ci.usage;
  if (VKD.vkCreateImage(g.device, &ci, nullptr, &t.image) != VK_SUCCESS) { std::fprintf(stderr, "acl-gpu: createImage fail\n"); return false; }

  const VkImageAspectFlagBits planeAspect[4] = {
      VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
  for (uint32_t i = 0; i < plane_count; ++i) {
    VkImageSubresource sub{static_cast<VkImageAspectFlags>(planeAspect[i]), 0, 0};
    VkSubresourceLayout lo{};
    VKD.vkGetImageSubresourceLayout(g.device, t.image, &sub, &lo);
    out->offsets[i] = static_cast<uint32_t>(lo.offset);
    out->strides[i] = static_cast<uint32_t>(lo.rowPitch);
  }

  VkMemoryRequirements reqs{};
  VKD.vkGetImageMemoryRequirements(g.device, t.image, &reqs);
  VkPhysicalDeviceMemoryProperties mp{};
  VKD.vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
  uint32_t mt = UINT32_MAX;
  for (uint32_t j = 0; j < mp.memoryTypeCount; ++j) {
    if (!(reqs.memoryTypeBits & (1u << j))) continue;
    if (mp.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { mt = j; break; }
    if (mt == UINT32_MAX) mt = j;
  }
  if (mt == UINT32_MAX) return false;
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.image = t.image;
  VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded,
                                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  t.mem_size = reqs.size;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, reqs.size, mt};
  if (VKD.vkAllocateMemory(g.device, &mai, nullptr, &t.memory) != VK_SUCCESS) { std::fprintf(stderr, "acl-gpu: allocMem fail\n"); return false; }
  if (VKD.vkBindImageMemory(g.device, t.image, t.memory, 0) != VK_SUCCESS) { std::fprintf(stderr, "acl-gpu: bind fail\n"); return false; }

  VkMemoryGetFdInfoKHR fdi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, t.memory,
                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  if (VKD.vkGetMemoryFdKHR(g.device, &fdi, &t.fd) != VK_SUCCESS) { std::fprintf(stderr, "acl-gpu: getFd fail\n"); return false; }

  out->fd = t.fd;
  out->width = static_cast<uint32_t>(w);
  out->height = static_cast<uint32_t>(h);
  out->plane_count = plane_count;
  out->modifier = modifier;
  out->drm_format = kDrmFormat;
  t.allocated = true;
  return true;
}

bool ensure_ready(int w, int h) {
  if (g.ready && g.w == w && g.h == h) return true;
  if (g.ready) return false;
  if (!init_device()) return false;

  // Skia gets its Vulkan entrypoints from the SAME dispatcher — consistent dispatch.
  GrVkGetProc gp = [](const char* name, VkInstance inst, VkDevice dev) -> PFN_vkVoidFunction {
    if (dev != VK_NULL_HANDLE) return VKD.vkGetDeviceProcAddr(dev, name);
    return VKD.vkGetInstanceProcAddr(inst, name);
  };
  g.ext.init(gp, g.instance, g.phys, 0, nullptr, 0, nullptr);
  GrVkBackendContext bc{};
  bc.fInstance = g.instance;
  bc.fPhysicalDevice = g.phys;
  bc.fDevice = g.device;
  bc.fQueue = g.queue;
  bc.fGraphicsQueueIndex = g.qfam;
  bc.fMaxAPIVersion = VK_API_VERSION_1_1;
  bc.fVkExtensions = &g.ext;
  bc.fGetProc = gp;
  g.gr = GrDirectContext::MakeVulkan(bc);
  if (!g.gr) { std::fprintf(stderr, "acl-gpu: MakeVulkan fail\n"); return false; }

  // kTopLeft origin: Skia renders top-down so the raw vkCmdCopyImage into the (top-down)
  // dma-buf keeps the right way up. The default (kBottomLeft) renders bottom-up and presents
  // vertically flipped.
  g.opt = SkSurface::MakeRenderTarget(
      g.gr.get(), SkBudgeted::kYes,
      SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType), 0,
      kTopLeft_GrSurfaceOrigin, nullptr);
  if (!g.opt) { std::fprintf(stderr, "acl-gpu: MakeRenderTarget fail\n"); return false; }

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = g.qfam;
  if (VKD.vkCreateCommandPool(g.device, &pci, nullptr, &g.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbi.commandPool = g.pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  if (VKD.vkAllocateCommandBuffers(g.device, &cbi, &g.cmd) != VK_SUCCESS) return false;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (VKD.vkCreateFence(g.device, &fci, nullptr, &g.fence) != VK_SUCCESS) return false;

  g.w = w;
  g.h = h;
  g.ready = true;
  return true;
}

// Copy the just-rendered optimal Skia image (TRANSFER_SRC_OPTIMAL) into `t`'s dma-buf image.
bool copy_to_dmabuf(Target& t) {
  GrBackendTexture bt = g.opt->getBackendTexture(SkSurface::kFlushRead_BackendHandleAccess);
  GrVkImageInfo si{};
  if (!bt.getVkImageInfo(&si)) { std::fprintf(stderr, "acl-gpu: getVkImageInfo fail\n"); return false; }

  VKD.vkResetCommandBuffer(g.cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKD.vkBeginCommandBuffer(g.cmd, &bi);
  const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.image = t.image;
  toDst.subresourceRange = color;
  VKD.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {static_cast<uint32_t>(g.w), static_cast<uint32_t>(g.h), 1};
  VKD.vkCmdCopyImage(g.cmd, si.fImage, static_cast<VkImageLayout>(si.fImageLayout), t.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  VkImageMemoryBarrier toExt{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toExt.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toExt.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  toExt.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  toExt.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toExt.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toExt.image = t.image;
  toExt.subresourceRange = color;
  VKD.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &toExt);
  VKD.vkEndCommandBuffer(g.cmd);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &g.cmd;
  if (VKD.vkQueueSubmit(g.queue, 1, &submit, g.fence) != VK_SUCCESS) return false;
  VKD.vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
  VKD.vkResetFences(g.device, 1, &g.fence);
  return true;
}

// Direct (zero-copy) path: wrap the modifier-tiled dma-buf VkImage itself as a Ganesh
// SkSurface so Skia renders STRAIGHT into the exported image — no optimal surface, no
// vkCmdCopyImage. Whether android-13 GrVkCaps accepts a DRM_FORMAT_MODIFIER image as a
// render target is the open question this path answers; falls back to the copy path if not.
bool make_direct_surface() {
  if (g.direct) return true;
  Target& t = g.targets[0];
  GrVkAlloc alloc;
  alloc.fMemory = t.memory;
  alloc.fOffset = 0;
  alloc.fSize = t.mem_size;
  alloc.fFlags = 0;
  GrVkImageInfo info{};
  info.fImage = t.image;
  info.fAlloc = alloc;
  info.fImageTiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info.fFormat = kVkFormat;
  info.fImageUsageFlags = g.usage;
  info.fSampleCount = 1;
  info.fLevelCount = 1;
  info.fCurrentQueueFamily = g.qfam;
  info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  GrBackendTexture bt(g.w, g.h, info);
  g.direct = SkSurface::MakeFromBackendTexture(g.gr.get(), bt, kTopLeft_GrSurfaceOrigin, 1,
                                               kBGRA_8888_SkColorType, nullptr, nullptr);
  if (!g.direct) {
    std::fprintf(stderr, "acl-gpu: MakeFromBackendTexture(direct tiled) fail\n");
    return false;
  }
  return true;
}

}  // namespace

// Render a scene GPU-native into a modifier-tiled dma-buf and export it; acl presents the fd.
extern "C" bool acl_graphics_gpu_render(int w, int h, uint32_t bg, int l, int t, int r,
                                        int b, uint32_t fg, AclDmabuf* out) {
  if (out == nullptr) return false;
  std::memset(out, 0, sizeof(*out));
  out->fd = -1;
  if (!ensure_ready(w, h)) return false;
  if (!g.targets[0].allocated && !make_target(g.targets[0], w, h)) return false;
  *out = g.targets[0].info;

  // Prefer the zero-copy direct path when ACL_GPU_DIRECT is set AND Skia accepts the
  // modifier-tiled image as a render target; otherwise use the proven copy path.
  const bool want_direct = std::getenv("ACL_GPU_DIRECT") != nullptr;
  if (want_direct && make_direct_surface()) {
    SkCanvas* canvas = g.direct->getCanvas();
    canvas->clear(static_cast<SkColor>(bg));
    SkPaint paint;
    paint.setColor(static_cast<SkColor>(fg));
    paint.setAntiAlias(false);
    canvas->drawRect(SkRect::MakeLTRB(l, t, r, b), paint);
    // Hand the image to the compositor in GENERAL layout, released from our queue family.
    GrBackendSurfaceMutableState toGeneral(VK_IMAGE_LAYOUT_GENERAL, g.qfam);
    g.direct->flush(GrFlushInfo{}, &toGeneral);
    g.gr->submit(/*syncCpu=*/true);
    return out->fd >= 0;
  }

  SkCanvas* canvas = g.opt->getCanvas();
  canvas->clear(static_cast<SkColor>(bg));
  SkPaint paint;
  paint.setColor(static_cast<SkColor>(fg));
  paint.setAntiAlias(false);
  canvas->drawRect(SkRect::MakeLTRB(l, t, r, b), paint);

  GrBackendSurfaceMutableState toSrc(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.qfam);
  g.opt->flush(GrFlushInfo{}, &toSrc);
  g.gr->submit(/*syncCpu=*/true);
  if (!copy_to_dmabuf(g.targets[0])) return false;
  return out->fd >= 0;
}

extern "C" bool acl_graphics_gpu_available() { return true; }

// ── S4b: drive the app's REAL android.graphics.Canvas onto the GPU surface ──────────
// begin_frame prepares the GPU render target and returns its SkCanvas as a handle so the
// runtime can wrap it in `new android.graphics.Canvas(long)`; the app then issues real
// Canvas ops (drawColor/drawRect/…) that land on the GPU surface. end_frame flushes Ganesh
// and vkCmdCopyImages the result into the modifier-tiled dma-buf (same copy path as the
// hardcoded renderer). The returned SkCanvas is owned by the SkSurface, so it is marked
// "borrowed" and graphics_jni's Canvas finalizer must NOT delete it.
namespace {
std::unordered_set<void*> g_borrowed_canvas;
}  // namespace

extern "C" bool acl_graphics_canvas_borrowed(void* c) {
  return g_borrowed_canvas.count(c) != 0;
}

extern "C" long acl_graphics_gpu_begin_frame(int w, int h) {
  if (!ensure_ready(w, h)) return 0;
  SkCanvas* c = g.opt->getCanvas();
  c->clear(SK_ColorTRANSPARENT);
  g_borrowed_canvas.insert(c);
  return reinterpret_cast<long>(c);
}

// Flush the shared optimal surface and copy it into dma-buf image `buffer_index` (0..1),
// returning that buffer's descriptor. The animated loop alternates buffer_index so the
// compositor can hold one image while the next is rendered — no in-flight overwrite.
// DIAG: dump the just-flushed optimal surface to a PPM at $ACL_GPU_DUMP (overwritten each frame, so
// the file holds the most-recent presented frame). PPM keeps this encoder-free; convert externally.
static void maybe_dump_frame() {
  const char* path = std::getenv("ACL_GPU_DUMP");
  if (path == nullptr || *path == '\0' || !g.opt) return;
  const int w = g.w, h = g.h;
  std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h);
  SkImageInfo info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
  if (!g.opt->readPixels(info, px.data(), static_cast<std::size_t>(w) * 4, 0, 0)) return;
  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  std::vector<std::uint8_t> row(static_cast<std::size_t>(w) * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      std::uint32_t p = px[static_cast<std::size_t>(y) * w + x];  // BGRA in memory (little-endian)
      row[x * 3 + 0] = static_cast<std::uint8_t>((p >> 16) & 0xff);  // R
      row[x * 3 + 1] = static_cast<std::uint8_t>((p >> 8) & 0xff);   // G
      row[x * 3 + 2] = static_cast<std::uint8_t>(p & 0xff);          // B
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

// Deferred-present replay: when the UI thread records the frame into an SkPicture (ACL_PRESENT_RT),
// replay it onto the GPU canvas here, just before flush. No-op in the default immediate path.
extern "C" void acl_graphics_present_replay_onto(void* sk_canvas);

extern "C" bool acl_graphics_gpu_end_frame(int buffer_index, AclDmabuf* out) {
  if (out == nullptr || !g.ready || !g.opt) return false;
  if (buffer_index < 0 || buffer_index >= kBuffers) buffer_index = 0;
  Target& t = g.targets[buffer_index];
  if (!t.allocated && !make_target(t, g.w, g.h)) return false;
  acl_graphics_present_replay_onto(g.opt->getCanvas());
  GrBackendSurfaceMutableState toSrc(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.qfam);
  g.opt->flush(GrFlushInfo{}, &toSrc);
  g.gr->submit(/*syncCpu=*/true);
  maybe_dump_frame();
  if (!copy_to_dmabuf(t)) return false;
  *out = t.info;
  return out->fd >= 0;
}

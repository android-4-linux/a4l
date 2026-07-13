// SPDX-License-Identifier: Apache-2.0
// Provenance: acl P2.4/S4a; ports acl gfx_vk render_to_modifier_dmabuf (Apache-2.0,
// vulkan-hpp) to raw Vulkan + Android Skia Ganesh; Skia public Ganesh Vulkan API. No GPL-3.0 sources.
//
// GPU-native zero-copy render (S4a step 2): render a scene with Android's (now Vulkan-
// enabled) host Skia Ganesh DIRECTLY into a modifier-tiled, dma-buf-exported VkImage — no
// CPU upload. acl presents the exported fd via zwp_linux_dmabuf_v1 (V2b). Self-contained:
// vulkan via dlopen(libvulkan)+GrVkGetProc (Soong can't -lvulkan; Skia only needs the
// proc), so no loader link and no VkDevice shared with acl.

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

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

constexpr VkFormat kVkFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kDrmFormat = DRM_FORMAT_XRGB8888;

// The persistent GPU render context (built once; keeps the dma-buf fd alive for present).
struct Gpu {
  void* loader = nullptr;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t qfam = UINT32_MAX;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  int fd = -1;
  sk_sp<GrDirectContext> gr;
  sk_sp<SkSurface> surface;
  GrVkExtensions ext;
  AclDmabuf info{};
  int w = 0, h = 0;
  bool ready = false;
} g;

template <class F>
F ldi(VkInstance inst, const char* name) {
  return reinterpret_cast<F>(g.gipa(inst, name));
}
template <class F>
F ldd(const char* name) {
  return reinterpret_cast<F>(g.gdpa(g.device, name));
}

bool init_device() {
  g.loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (g.loader == nullptr) g.loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (g.loader == nullptr) { std::fprintf(stderr,"acl-gpu: no libvulkan\n"); return false; }
  g.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(g.loader, "vkGetInstanceProcAddr"));
  if (g.gipa == nullptr) return false;

  auto vkCreateInstance = ldi<PFN_vkCreateInstance>(nullptr, "vkCreateInstance");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) { std::fprintf(stderr,"acl-gpu: createInstance fail\n"); return false; }

  auto vkEnumeratePhysicalDevices =
      ldi<PFN_vkEnumeratePhysicalDevices>(g.instance, "vkEnumeratePhysicalDevices");
  auto vkGetPhysicalDeviceProperties =
      ldi<PFN_vkGetPhysicalDeviceProperties>(g.instance, "vkGetPhysicalDeviceProperties");
  auto vkGetPhysicalDeviceQueueFamilyProperties =
      ldi<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          g.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  auto vkCreateDevice = ldi<PFN_vkCreateDevice>(g.instance, "vkCreateDevice");
  g.gdpa = ldi<PFN_vkGetDeviceProcAddr>(g.instance, "vkGetDeviceProcAddr");

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
  if (n == 0) return false;
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(g.instance, &n, devs.data());
  g.phys = devs[0];
  for (auto d : devs) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
        p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) { g.phys = d; break; }
  }
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qfs.data());
  for (uint32_t i = 0; i < qn; ++i)
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.qfam = i; break; }
  if (g.qfam == UINT32_MAX) return false;

  const char* exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME};
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = g.qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 4;
  dci.ppEnabledExtensionNames = exts;
  if (vkCreateDevice(g.phys, &dci, nullptr, &g.device) != VK_SUCCESS) { std::fprintf(stderr,"acl-gpu: createDevice fail\n"); return false; }
  ldd<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(g.device, g.qfam, 0, &g.queue);
  return true;
}

// Pick a GPU render+sampleable modifier for kVkFormat (prefer tiled over LINEAR).
uint64_t choose_modifier(uint32_t* plane_count) {
  auto vkGetPhysicalDeviceFormatProperties2 =
      ldi<PFN_vkGetPhysicalDeviceFormatProperties2>(
          g.instance, "vkGetPhysicalDeviceFormatProperties2");
  VkDrmFormatModifierPropertiesListEXT list{
      VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 fp2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
  vkGetPhysicalDeviceFormatProperties2(g.phys, kVkFormat, &fp2);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  vkGetPhysicalDeviceFormatProperties2(g.phys, kVkFormat, &fp2);
  constexpr VkFormatFeatureFlags need =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  // Prefer a SINGLE-plane tiled modifier: Ganesh wraps one VkImage as one GrVkImageInfo,
  // so multi-plane modifiers (e.g. AMD DCC's color+metadata) can't be expressed. Fall back
  // to any single-plane modifier, then LINEAR.
  uint64_t tiled1 = DRM_FORMAT_MOD_INVALID, linear = DRM_FORMAT_MOD_INVALID;
  for (const auto& m : mods) {
    if ((m.drmFormatModifierTilingFeatures & need) != need) continue;
    if (m.drmFormatModifierPlaneCount != 1) continue;  // no DCC/multi-plane
    if (m.drmFormatModifier == DRM_FORMAT_MOD_LINEAR) { linear = m.drmFormatModifier; continue; }
    if (tiled1 == DRM_FORMAT_MOD_INVALID) tiled1 = m.drmFormatModifier;  // first single-plane tiled
  }
  if (tiled1 != DRM_FORMAT_MOD_INVALID) { *plane_count = 1; return tiled1; }
  if (linear != DRM_FORMAT_MOD_INVALID) { *plane_count = 1; return linear; }
  return DRM_FORMAT_MOD_INVALID;
}

// Create the modifier-tiled, dma-buf-exported VkImage + memory + fd; wrap it as a Ganesh
// SkSurface. Fills the plane layout into `out`.
//
// FINDING (S4a): android-13 Ganesh's SkSurface::MakeFromBackendTexture REJECTS a
// DRM_FORMAT_MODIFIER-tiled image as a render target (both tiled AND linear modifiers) —
// GrVkCaps lacks the modifier render-target support m150 has. So direct GPU-native render
// into the dma-buf isn't possible here; the fix (TODO) is the gfx_vk copy pattern: render
// into a Skia-owned OPTIMAL SkSurface, then vkCmdCopyImage into this dma-buf image
// (GPU-to-GPU, still no CPU upload), then export. Everything up to this wrap works.
bool make_target(int w, int h, AclDmabuf* out) {
  uint32_t plane_count = 1;
  const uint64_t modifier = choose_modifier(&plane_count);
  std::fprintf(stderr,"acl-gpu: modifier=0x%llx planes=%u\n",(unsigned long long)modifier,plane_count);
  if (modifier == DRM_FORMAT_MOD_INVALID) { std::fprintf(stderr,"acl-gpu: no modifier\n"); return false; }
  if (plane_count > 4) plane_count = 4;

  auto vkCreateImage = ldd<PFN_vkCreateImage>("vkCreateImage");
  auto vkGetImageMemoryRequirements =
      ldd<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
  auto vkGetImageSubresourceLayout =
      ldd<PFN_vkGetImageSubresourceLayout>("vkGetImageSubresourceLayout");
  auto vkAllocateMemory = ldd<PFN_vkAllocateMemory>("vkAllocateMemory");
  auto vkBindImageMemory = ldd<PFN_vkBindImageMemory>("vkBindImageMemory");
  auto vkGetMemoryFdKHR = ldd<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR");
  auto vkGetPhysicalDeviceMemoryProperties = ldi<PFN_vkGetPhysicalDeviceMemoryProperties>(
      g.instance, "vkGetPhysicalDeviceMemoryProperties");

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
  if (vkCreateImage(g.device, &ci, nullptr, &g.image) != VK_SUCCESS) { std::fprintf(stderr,"acl-gpu: createImage fail\n"); return false; }

  const VkImageAspectFlagBits planeAspect[4] = {
      VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
  for (uint32_t i = 0; i < plane_count; ++i) {
    VkImageSubresource sub{static_cast<VkImageAspectFlags>(planeAspect[i]), 0, 0};
    VkSubresourceLayout lo{};
    vkGetImageSubresourceLayout(g.device, g.image, &sub, &lo);
    out->offsets[i] = static_cast<uint32_t>(lo.offset);
    out->strides[i] = static_cast<uint32_t>(lo.rowPitch);
  }

  VkMemoryRequirements reqs{};
  vkGetImageMemoryRequirements(g.device, g.image, &reqs);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
  uint32_t mt = UINT32_MAX;
  for (uint32_t j = 0; j < mp.memoryTypeCount; ++j) {
    if (!(reqs.memoryTypeBits & (1u << j))) continue;
    if (mp.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { mt = j; break; }
    if (mt == UINT32_MAX) mt = j;
  }
  if (mt == UINT32_MAX) return false;
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.image = g.image;
  VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded,
                                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, reqs.size, mt};
  if (vkAllocateMemory(g.device, &mai, nullptr, &g.memory) != VK_SUCCESS) { std::fprintf(stderr,"acl-gpu: allocMem fail\n"); return false; }
  if (vkBindImageMemory(g.device, g.image, g.memory, 0) != VK_SUCCESS) { std::fprintf(stderr,"acl-gpu: bind fail\n"); return false; }

  VkMemoryGetFdInfoKHR fdi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, g.memory,
                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  if (vkGetMemoryFdKHR(g.device, &fdi, &g.fd) != VK_SUCCESS) { std::fprintf(stderr,"acl-gpu: getFd fail\n"); return false; }

  out->fd = g.fd;
  out->width = static_cast<uint32_t>(w);
  out->height = static_cast<uint32_t>(h);
  out->plane_count = plane_count;
  out->modifier = modifier;
  out->drm_format = kDrmFormat;

  // Wrap the VkImage as an Android-Skia Ganesh SkSurface.
  GrVkImageInfo info{};
  info.fImage = g.image;
  info.fAlloc.fMemory = g.memory;
  info.fAlloc.fOffset = 0;
  info.fAlloc.fSize = reqs.size;
  info.fImageTiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info.fFormat = kVkFormat;
  info.fImageUsageFlags = ci.usage;
  info.fSampleCount = 1;
  info.fLevelCount = 1;
  info.fCurrentQueueFamily = g.qfam;
  GrBackendTexture bt(w, h, info);
  g.surface = SkSurface::MakeFromBackendTexture(
      g.gr.get(), bt, kTopLeft_GrSurfaceOrigin, /*sampleCnt=*/1, kBGRA_8888_SkColorType,
      nullptr, nullptr);
  if (!g.surface) std::fprintf(stderr,"acl-gpu: MakeFromBackendTexture fail\n");
  return g.surface != nullptr;
}

bool ensure_ready(int w, int h) {
  if (g.ready && g.w == w && g.h == h) return true;
  if (g.ready) return false;  // fixed size for now
  if (!init_device()) return false;

  GrVkGetProc gp = [](const char* name, VkInstance inst, VkDevice dev) -> PFN_vkVoidFunction {
    if (dev != VK_NULL_HANDLE) return g.gdpa(dev, name);
    return g.gipa(inst, name);
  };
  const char* exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME};
  g.ext.init(gp, g.instance, g.phys, 0, nullptr, 4, exts);
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
  if (!g.gr) { std::fprintf(stderr,"acl-gpu: MakeVulkan fail\n"); return false; }
  g.w = w;
  g.h = h;
  g.ready = true;
  return true;
}

}  // namespace

// Render a scene GPU-native into a modifier-tiled dma-buf and export it. acl presents the
// fd via zwp_linux_dmabuf_v1. Returns false on failure.
extern "C" bool acl_graphics_gpu_render(int w, int h, uint32_t bg, int l, int t, int r,
                                        int b, uint32_t fg, AclDmabuf* out) {
  if (out == nullptr) return false;
  std::memset(out, 0, sizeof(*out));
  out->fd = -1;
  if (!ensure_ready(w, h)) return false;
  if (!g.surface) {
    if (!make_target(w, h, &g.info)) return false;
  }
  *out = g.info;

  SkCanvas* canvas = g.surface->getCanvas();
  canvas->clear(static_cast<SkColor>(bg));
  SkPaint paint;
  paint.setColor(static_cast<SkColor>(fg));
  paint.setAntiAlias(false);
  canvas->drawRect(SkRect::MakeLTRB(l, t, r, b), paint);

  // Flush + leave the image in GENERAL for the compositor to sample; block on the GPU so
  // the dma-buf holds a complete frame (implicit dma-buf sync on present).
  GrBackendSurfaceMutableState toPresent(VK_IMAGE_LAYOUT_GENERAL, g.qfam);
  g.surface->flush(GrFlushInfo{}, &toPresent);
  g.gr->submit(/*syncCpu=*/true);
  return out->fd >= 0;
}

extern "C" bool acl_graphics_gpu_available() { return true; }

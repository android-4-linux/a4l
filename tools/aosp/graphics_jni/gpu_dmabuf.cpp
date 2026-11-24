// SPDX-License-Identifier: Apache-2.0
// Provenance: acl P2.4/S4a; ports acl gfx_vk render_to_modifier_dmabuf
// (Apache-2.0, vulkan-hpp); Vulkan-Hpp dynamic-dispatcher pattern from
// ivi-homescreen vulkan_drm_backend.cc (Apache-2.0); Skia public Ganesh Vulkan
// API. No GPL-3.0 sources.
//
// GPU-native render (S4a step 2): render the scene with Android's (now
// Vulkan-enabled) host Skia Ganesh into a Skia-owned OPTIMAL surface, then
// vkCmdCopyImage it into a modifier-tiled, dma-buf-exported VkImage (GPU->GPU,
// no CPU upload). acl presents the exported fd via zwp_linux_dmabuf_v1 (V2b).
//
// Loader: Vulkan-Hpp's DEFAULT dynamic dispatcher — init() [internal
// DynamicLoader], then init(instance), then init(device) — so EVERY Vulkan call
// (ours AND Skia's, via a GrVkGetProc sourced from the same dispatcher) uses
// ONE consistent dispatch. A separate dlopen + ad-hoc vkGetDeviceProcAddr gave
// an inconsistent device dispatch under ART
// ("VkDevice dispatch handle not found") that silently broke Skia's render
// targets.

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_EXCEPTIONS  // Soong builds -fno-exceptions
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <drm_fourcc.h>

#include "GrBackendSurface.h"
#include "GrBackendSurfaceMutableState.h"
#include "GrDirectContext.h"
#include "SkCanvas.h"
#include "SkColor.h"
#include "SkColorSpace.h"
#include "SkImage.h"
#include "SkImageInfo.h"
#include "SkPaint.h"
#include "SkRect.h"
#include "SkSurface.h"
#include "gpu/vk/GrVkBackendContext.h"
#include "gpu/vk/GrVkExtensions.h"
#include "gpu/vk/GrVkTypes.h"
#include "st_frame.h"

extern "C" struct AclDmabuf {
  int fd;
  uint32_t width, height, plane_count;
  uint32_t offsets[4], strides[4];
  uint64_t modifier;
  uint32_t drm_format;
};

namespace {

#define VKD VULKAN_HPP_DEFAULT_DISPATCHER  // the one dispatcher everything uses

// The present format. ARGB (not XRGB) so the compositor honors the alpha
// channel: the video SurfaceView hole-punch clears its rect to transparent and
// the video wl_subsurface below shows through. Opaque apps render alpha=255 so
// they look identical; the presenter marks the opaque region (full minus the
// video hole) so the compositor still skips blending elsewhere.
//
// BGRA is the default. The byte order is a choice because a driver may offer
// its tiled modifier for one order only: Adreno 640 lists a vendor modifier
// for R8G8B8A8 and LINEAR alone for B8G8R8A8. choose_format() settles it once,
// before the first surface is made, and leaves BGRA unless the presenter
// offered the other order.
struct PresentFormat {
  VkFormat vk;
  uint32_t drm;
  SkColorType ct;
};
constexpr PresentFormat kBgra = {VK_FORMAT_B8G8R8A8_UNORM, DRM_FORMAT_ARGB8888,
                                 kBGRA_8888_SkColorType};
constexpr PresentFormat kRgba = {VK_FORMAT_R8G8B8A8_UNORM, DRM_FORMAT_ABGR8888,
                                 kRGBA_8888_SkColorType};
PresentFormat g_fmt = kBgra;
uint64_t g_modifier = DRM_FORMAT_MOD_INVALID;  // chosen with g_fmt
bool g_fmt_chosen = false;

// One exported, modifier-tiled dma-buf VkImage + its memory/fd/descriptor. The
// animated present loop ping-pongs two of these so the compositor can hold one
// for scanout while Skia renders the next — see
// acl_graphics_gpu_end_frame(buffer_index, ...).
struct Target {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize mem_size = 0;
  int fd = -1;
  AclDmabuf info{};
  bool allocated = false;
};

// 4 ping-pong dma-buf targets. The deferred RenderThread pipeline needs one
// displayed by the compositor, one being rendered, and one in transit — 2
// starves (buffer collisions -> flicker). The fourth is for a compositor that
// releases a replaced buffer only when the frame after it has been shown
// (Weston 8 with the buffer on a hardware plane, about two refreshes): three
// are then held when the next frame needs one, and the app presents at half
// rate. A target is allocated on first use and the presenter takes the lowest
// free slot, so the fourth exists only where three are in fact held.
constexpr int kBuffers = 4;

struct Gpu {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t qfam = UINT32_MAX;
  // Enabled device extension list + features2 chain, kept for the WebView
  // AwDrawFn InitVk (hands Chromium the host device it must render Skia into —
  // see the VK functor path).
  const char* const* device_exts = nullptr;
  uint32_t device_ext_count = 0;
  VkPhysicalDeviceFeatures2* features2 = nullptr;
  VkImageUsageFlags usage = 0;
  Target targets[kBuffers];  // ping-pong dma-buf images (copy destinations)
  sk_sp<GrDirectContext> gr;
  sk_sp<SkSurface>
      opt;  // single Skia-owned optimal render target (copy-path src)
  sk_sp<SkSurface>
      direct;  // a dma-buf VkImage wrapped as SkSurface (zero-copy direct path)
  GrVkExtensions ext;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  int w = 0, h = 0;
  bool ready = false;
  // Swapchain present (acl_graphics_gpu_set_wayland_surface): the driver's
  // WSI owns the window's buffers and commits the wl_surface.
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::vector<VkImage> sc_images;
  VkColorSpaceKHR sc_colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  VkFence sc_fence = VK_NULL_HANDLE;
  int sc_w = 0, sc_h = 0;
} g;

// Whether the present device draws a decoder's frame as it is
// (acl_gpu_draws_yuv): set once the Skia context is made.
std::atomic<bool> g_draws_yuv{false};

// The Wayland surface to present on, set before the device is made. Both
// null: the dma-buf path.
void* g_wl_display = nullptr;
void* g_wl_surface = nullptr;
bool g_swapchain_mode = false;  // the surface was made and can be presented to

bool has_extension(const std::vector<VkExtensionProperties>& exts,
                   const char* name) {
  for (const auto& e : exts)
    if (std::strcmp(e.extensionName, name) == 0)
      return true;
  return false;
}

// VkWaylandSurfaceCreateInfoKHR, declared here so that the file does not need
// the Wayland platform headers. The two pointers are a4l's own wl_display and
// wl_surface.
struct WaylandSurfaceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* display;
  void* surface;
};
constexpr VkStructureType kWaylandSurfaceCreateInfoType =
    static_cast<VkStructureType>(1000006000);
using CreateWaylandSurfaceFn = VkResult (*)(VkInstance,
                                            const WaylandSurfaceCreateInfo*,
                                            const VkAllocationCallbacks*,
                                            VkSurfaceKHR*);

// A VkSurfaceKHR on a4l's wl_surface that the device and its graphics queue
// can present to. Needs the instance, the physical device and the queue
// family. False: the dma-buf path.
bool make_wl_surface() {
  auto create = reinterpret_cast<CreateWaylandSurfaceFn>(
      g.gipa(g.instance, "vkCreateWaylandSurfaceKHR"));
  if (create == nullptr)
    return false;
  uint32_t en = 0;
  VKD.vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, nullptr);
  std::vector<VkExtensionProperties> exts(en);
  VKD.vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, exts.data());
  exts.resize(std::min<size_t>(exts.size(), en));
  if (!has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    std::fprintf(stderr, "acl-gpu: no VK_KHR_swapchain; presenting dma-bufs\n");
    return false;
  }
  WaylandSurfaceCreateInfo ci{kWaylandSurfaceCreateInfoType, nullptr, 0,
                              g_wl_display, g_wl_surface};
  if (create(g.instance, &ci, nullptr, &g.surface) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: createWaylandSurface fail\n");
    return false;
  }
  VkBool32 ok = VK_FALSE;
  if (VKD.vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.qfam, g.surface,
                                               &ok) != VK_SUCCESS ||
      ok != VK_TRUE) {
    std::fprintf(stderr, "acl-gpu: the queue cannot present to the surface\n");
    VKD.vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
    g.surface = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool init_device() {
  // This vulkan-headers has no no-arg init(); feed it the loader's
  // vkGetInstanceProcAddr, then re-init at instance and device scope so ALL
  // dispatch is consistent.
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr)
    lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) {
    std::fprintf(stderr, "acl-gpu: no libvulkan\n");
    return false;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(lib, "vkGetInstanceProcAddr"));
  if (gipa == nullptr) {
    std::fprintf(stderr, "acl-gpu: no vkGetInstanceProcAddr\n");
    return false;
  }
  VKD.init(gipa);  // loads global entrypoints (vkCreateInstance, ...) via
                   // gipa(nullptr, name)
  if (VKD.vkCreateInstance == nullptr) {
    std::fprintf(stderr, "acl-gpu: no vulkan loader\n");
    return false;
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  g.gipa = gipa;
  static const char* const kSurfaceExts[] = {"VK_KHR_surface",
                                             "VK_KHR_wayland_surface"};
  bool want_surface = g_wl_display != nullptr && g_wl_surface != nullptr;
  if (want_surface) {
    uint32_t en = 0;
    VKD.vkEnumerateInstanceExtensionProperties(nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    VKD.vkEnumerateInstanceExtensionProperties(nullptr, &en, exts.data());
    exts.resize(std::min<size_t>(exts.size(), en));
    want_surface = has_extension(exts, kSurfaceExts[0]) &&
                   has_extension(exts, kSurfaceExts[1]);
    if (want_surface) {
      ici.enabledExtensionCount = 2;
      ici.ppEnabledExtensionNames = kSurfaceExts;
    } else {
      std::fprintf(stderr,
                   "acl-gpu: no VK_KHR_wayland_surface; presenting dma-bufs\n");
    }
  }
  if (VKD.vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: createInstance fail\n");
    return false;
  }
  VKD.init(vk::Instance(g.instance));  // load ALL instance-level entrypoints

  uint32_t n = 0;
  VKD.vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
  if (n == 0)
    return false;
  std::vector<VkPhysicalDevice> devs(n);
  VKD.vkEnumeratePhysicalDevices(g.instance, &n, devs.data());
  g.phys = devs[0];
  for (auto d : devs) {
    VkPhysicalDeviceProperties p;
    VKD.vkGetPhysicalDeviceProperties(d, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
        p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      g.phys = d;
      break;
    }
  }
  uint32_t qn = 0;
  VKD.vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qn);
  VKD.vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qfs.data());
  for (uint32_t i = 0; i < qn; ++i)
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      g.qfam = i;
      break;
    }
  if (g.qfam == UINT32_MAX)
    return false;

  // Kept STATIC: the Chromium WebView AwDrawFn Vulkan functor's InitVk takes
  // the host's device + enabled extension list + features2 and builds Skia's
  // GrVkBackendContext from them (which may keep the pointers), so this list
  // and the features2 chain must outlive device creation. The added
  // VK_KHR_external_semaphore_fd + the ycbcr-conversion feature mirror what
  // hwui's VulkanManager enables and what Chromium's per-frame semaphore
  // handshake / AHardwareBuffer sampling need.
  // VK_KHR_swapchain is last and counted only in swapchain mode.
  static const char* const kDeviceExts[] = {
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };
  const bool with_swapchain = want_surface && make_wl_surface();
  static VkPhysicalDeviceSamplerYcbcrConversionFeatures kYcbcr{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
      nullptr, VK_TRUE};
  static VkPhysicalDeviceFeatures2 kFeatures2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &kYcbcr, {}};
  g.device_exts = kDeviceExts;
  g.device_ext_count =
      static_cast<uint32_t>(std::size(kDeviceExts)) - (with_swapchain ? 0 : 1);
  g.features2 = &kFeatures2;

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = g.qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &kFeatures2;  // features2 (ycbcr) in the pNext chain;
                            // pEnabledFeatures must stay null
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = g.device_ext_count;
  dci.ppEnabledExtensionNames = kDeviceExts;
  if (VKD.vkCreateDevice(g.phys, &dci, nullptr, &g.device) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: createDevice fail\n");
    return false;
  }
  VKD.init(vk::Device(
      g.device));  // load ALL device-level entrypoints (consistent dispatch)
  VKD.vkGetDeviceQueue(g.device, g.qfam, 0, &g.queue);
  g_swapchain_mode = with_swapchain;
  return true;
}

// Hand the present VkDevice + enabled extensions/features to the WebView
// AwDrawFn InitVk builder (webview_functor.cpp). Chromium renders its Skia into
// a secondary command buffer on THIS device, so its device must be the same one
// acl composites the frame with. Returns false until the device is up.
extern "C" bool acl_gpu_vk_handles(void** inst,
                                   void** phys,
                                   void** dev,
                                   void** q,
                                   uint32_t* qfam,
                                   const char* const** exts,
                                   uint32_t* n_exts,
                                   void** features2) {
  if (g.device == VK_NULL_HANDLE)
    return false;
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

// The modifiers the compositor advertises, for the default format and for the
// other byte order (empty: not told). Set before the first target is made.
std::mutex g_allowed_mu;
std::vector<uint64_t> g_allowed;
std::vector<uint64_t> g_allowed_rgba;

// What one format offers: single-plane modifiers the GPU can render and
// sample, split by whether the compositor's list has them.
struct ModifierScan {
  uint64_t tiled = DRM_FORMAT_MOD_INVALID;       // GPU's, and advertised
  uint64_t linear = DRM_FORMAT_MOD_INVALID;      // LINEAR, and advertised
  uint64_t any_tiled = DRM_FORMAT_MOD_INVALID;   // GPU's, advertised or not
  uint64_t any_linear = DRM_FORMAT_MOD_INVALID;  // LINEAR, advertised or not
};

// `allowed` empty means the compositor did not say, and everything passes.
ModifierScan scan_modifiers(VkFormat format,
                            const std::vector<uint64_t>& allowed) {
  VkDrmFormatModifierPropertiesListEXT list{
      VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 fp2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
  VKD.vkGetPhysicalDeviceFormatProperties2(g.phys, format, &fp2);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  VKD.vkGetPhysicalDeviceFormatProperties2(g.phys, format, &fp2);
  mods.resize(std::min<size_t>(mods.size(), list.drmFormatModifierCount));
  constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  ModifierScan r;
  for (const auto& m : mods) {
    if ((m.drmFormatModifierTilingFeatures & need) != need)
      continue;
    if (m.drmFormatModifierPlaneCount != 1)
      continue;
    const bool ok =
        allowed.empty() || std::find(allowed.begin(), allowed.end(),
                                     m.drmFormatModifier) != allowed.end();
    if (m.drmFormatModifier == DRM_FORMAT_MOD_LINEAR) {
      r.any_linear = m.drmFormatModifier;
      if (ok)
        r.linear = m.drmFormatModifier;
      continue;
    }
    if (r.any_tiled == DRM_FORMAT_MOD_INVALID)
      r.any_tiled = m.drmFormatModifier;
    if (ok && r.tiled == DRM_FORMAT_MOD_INVALID)
      r.tiled = m.drmFormatModifier;
  }
  return r;
}

// Settle the present format and its modifier, once. In order: a tiled modifier
// the compositor advertises for BGRA; one it advertises for RGBA, which is
// taken only when the presenter passed a list for that format (a4l does so
// under ACL_PRESENT_ALLOW_ABGR); LINEAR for BGRA on the same terms. A
// compositor whose list has none of the GPU's gets the GPU's own choice for
// BGRA, as before the list was consulted: some take it, and the rest show
// nothing either way.
void choose_format() {
  if (g_fmt_chosen)
    return;
  g_fmt_chosen = true;
  if (g_swapchain_mode) {
    // The byte order is the surface's: BGRA if it offers it, else RGBA. With
    // neither there is nothing to copy the frame into, and the dma-buf path
    // takes over.
    uint32_t n = 0;
    VKD.vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    VKD.vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n,
                                             fmts.data());
    fmts.resize(std::min<size_t>(fmts.size(), n));
    const PresentFormat* pick = nullptr;
    for (const PresentFormat* want : {&kBgra, &kRgba}) {
      for (const auto& f : fmts) {
        if (pick == nullptr && f.format == want->vk) {
          pick = want;
          g.sc_colorspace = f.colorSpace;
        }
      }
    }
    if (pick != nullptr) {
      g_fmt = *pick;
      return;
    }
    std::fprintf(stderr,
                 "acl-gpu: the surface offers no 8-bit RGBA format; "
                 "presenting dma-bufs\n");
    g_swapchain_mode = false;
  }
  std::vector<uint64_t> allowed, allowed_rgba;
  {
    std::lock_guard<std::mutex> lk(g_allowed_mu);
    allowed = g_allowed;
    allowed_rgba = g_allowed_rgba;
  }
  const ModifierScan bgra = scan_modifiers(kBgra.vk, allowed);
  g_fmt = kBgra;
  if (bgra.tiled != DRM_FORMAT_MOD_INVALID) {
    g_modifier = bgra.tiled;
    return;
  }
  if (!allowed_rgba.empty()) {
    const ModifierScan rgba = scan_modifiers(kRgba.vk, allowed_rgba);
    if (rgba.tiled != DRM_FORMAT_MOD_INVALID) {
      g_fmt = kRgba;
      g_modifier = rgba.tiled;
      std::fprintf(stderr,
                   "acl-gpu: presenting ABGR8888 with modifier 0x%llx (no "
                   "tiled modifier for ARGB8888)\n",
                   static_cast<unsigned long long>(g_modifier));
      return;
    }
  }
  if (bgra.linear != DRM_FORMAT_MOD_INVALID) {
    g_modifier = bgra.linear;
    return;
  }
  g_modifier = bgra.any_tiled != DRM_FORMAT_MOD_INVALID ? bgra.any_tiled
                                                        : bgra.any_linear;
  if (g_modifier != DRM_FORMAT_MOD_INVALID)
    std::fprintf(stderr,
                 "acl-gpu: the compositor advertises no modifier this GPU "
                 "renders to; using 0x%llx\n",
                 static_cast<unsigned long long>(g_modifier));
}

// The compositor's modifiers for ARGB8888, from the presenter. Called before
// the first frame; a format already chosen stays.
extern "C" void acl_graphics_gpu_set_modifiers(const uint64_t* mods, size_t n) {
  std::lock_guard<std::mutex> lk(g_allowed_mu);
  g_allowed.assign(mods, mods + (mods != nullptr ? n : 0));
}

// Present through a swapchain on a4l's wl_surface, where the driver has
// VK_KHR_wayland_surface: its WSI allocates the window's buffers and commits
// the surface, as an EGL window does. Must be called before the first frame;
// it has no effect once the device is made. Returns whether the request was
// taken, not whether the driver can do it: end_frame says that, by returning
// a frame with no dma-buf (fd -1).
extern "C" bool acl_graphics_gpu_set_wayland_surface(void* wl_display,
                                                     void* wl_surface) {
  if (g.ready || g.device != VK_NULL_HANDLE || wl_display == nullptr ||
      wl_surface == nullptr)
    return false;
  g_wl_display = wl_display;
  g_wl_surface = wl_surface;
  return true;
}

// The same for another format. Only ABGR8888 is used; the rest are ignored.
extern "C" void acl_graphics_gpu_set_format_modifiers(uint32_t drm_format,
                                                      const uint64_t* mods,
                                                      size_t n) {
  if (drm_format != DRM_FORMAT_ABGR8888)
    return;
  std::lock_guard<std::mutex> lk(g_allowed_mu);
  g_allowed_rgba.assign(mods, mods + (mods != nullptr ? n : 0));
}

// Create one modifier-tiled, dma-buf-exported VkImage + memory + fd into `t` (a
// plain VkImage, the vkCmdCopyImage DST — the scene is rendered into g.opt and
// copied here).
bool make_target(Target& t, int w, int h) {
  const uint32_t plane_count = 1;
  choose_format();
  const uint64_t modifier = g_modifier;
  if (modifier == DRM_FORMAT_MOD_INVALID) {
    std::fprintf(stderr, "acl-gpu: no modifier\n");
    return false;
  }
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
  ci.format = g_fmt.vk;
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
  if (VKD.vkCreateImage(g.device, &ci, nullptr, &t.image) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: createImage fail\n");
    return false;
  }

  const VkImageAspectFlagBits planeAspect[4] = {
      VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
  for (uint32_t i = 0; i < plane_count; ++i) {
    VkImageSubresource sub{static_cast<VkImageAspectFlags>(planeAspect[i]), 0,
                           0};
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
    if (!(reqs.memoryTypeBits & (1u << j)))
      continue;
    if (mp.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      mt = j;
      break;
    }
    if (mt == UINT32_MAX)
      mt = j;
  }
  if (mt == UINT32_MAX)
    return false;
  VkMemoryDedicatedAllocateInfo ded{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.image = t.image;
  VkExportMemoryAllocateInfo exp{
      VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  t.mem_size = reqs.size;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp,
                           reqs.size, mt};
  if (VKD.vkAllocateMemory(g.device, &mai, nullptr, &t.memory) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: allocMem fail\n");
    return false;
  }
  if (VKD.vkBindImageMemory(g.device, t.image, t.memory, 0) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: bind fail\n");
    return false;
  }

  VkMemoryGetFdInfoKHR fdi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr,
                           t.memory,
                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  if (VKD.vkGetMemoryFdKHR(g.device, &fdi, &t.fd) != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: getFd fail\n");
    return false;
  }

  out->fd = t.fd;
  out->width = static_cast<uint32_t>(w);
  out->height = static_cast<uint32_t>(h);
  out->plane_count = plane_count;
  out->modifier = modifier;
  out->drm_format = g_fmt.drm;
  t.allocated = true;
  return true;
}

// The window changed size: a new render target, and new dma-bufs the next
// time each is rendered into. Runs on the thread that owns the GrContext,
// between frames. The compositor may still be showing an old dma-buf; it
// holds its own reference, so destroying ours here takes nothing from it.
bool resize_targets(int w, int h) {
  if (w <= 0 || h <= 0)
    return false;
  g.gr->flushAndSubmit(/*syncCpu=*/true);
  sk_sp<SkSurface> opt = SkSurface::MakeRenderTarget(
      g.gr.get(), SkBudgeted::kYes,
      SkImageInfo::Make(w, h, g_fmt.ct, kPremul_SkAlphaType), 0,
      kTopLeft_GrSurfaceOrigin, nullptr);
  if (!opt) {
    std::fprintf(stderr, "acl-gpu: MakeRenderTarget %dx%d fail\n", w, h);
    return false;  // the old size stays usable
  }
  g.direct.reset();
  g.opt = std::move(opt);
  for (Target& t : g.targets) {
    if (t.image != VK_NULL_HANDLE)
      VKD.vkDestroyImage(g.device, t.image, nullptr);
    if (t.memory != VK_NULL_HANDLE)
      VKD.vkFreeMemory(g.device, t.memory, nullptr);
    if (t.fd >= 0)
      ::close(t.fd);
    t = Target{};
  }
  g.w = w;
  g.h = h;
  return true;
}

bool ensure_ready(int w, int h) {
  if (g.ready && g.w == w && g.h == h)
    return true;
  if (g.ready)
    return resize_targets(w, h);
  if (!init_device())
    return false;

  // Skia gets its Vulkan entrypoints from the SAME dispatcher — consistent
  // dispatch.
  GrVkGetProc gp = [](const char* name, VkInstance inst,
                      VkDevice dev) -> PFN_vkVoidFunction {
    if (dev != VK_NULL_HANDLE)
      return VKD.vkGetDeviceProcAddr(dev, name);
    return VKD.vkGetInstanceProcAddr(inst, name);
  };
  // Skia is told of one device extension, the DRM format modifier one: it
  // wraps an image made with a modifier (an imported decoder frame) only
  // when it knows the device has it.
  static const char* const kSkiaDeviceExts[] = {
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
  g.ext.init(gp, g.instance, g.phys, 0, nullptr, 1, kSkiaDeviceExts);
  GrVkBackendContext bc{};
  bc.fInstance = g.instance;
  bc.fPhysicalDevice = g.phys;
  bc.fDevice = g.device;
  bc.fQueue = g.queue;
  bc.fGraphicsQueueIndex = g.qfam;
  bc.fMaxAPIVersion = VK_API_VERSION_1_1;
  bc.fVkExtensions = &g.ext;
  // Skia builds its caps from that list only when it is also given the
  // device's features: none of the core ones, and the sampler YCbCr
  // conversion, with which it draws a decoder's frame as it is.
  bc.fDeviceFeatures2 = g.features2;
  bc.fGetProc = gp;
  g.gr = GrDirectContext::MakeVulkan(bc);
  if (!g.gr) {
    std::fprintf(stderr, "acl-gpu: MakeVulkan fail\n");
    return false;
  }
  g_draws_yuv.store(true, std::memory_order_release);

  choose_format();  // the surface below is made in the present format
  // kTopLeft origin: Skia renders top-down so the raw vkCmdCopyImage into the
  // (top-down) dma-buf keeps the right way up. The default (kBottomLeft)
  // renders bottom-up and presents vertically flipped.
  g.opt = SkSurface::MakeRenderTarget(
      g.gr.get(), SkBudgeted::kYes,
      SkImageInfo::Make(w, h, g_fmt.ct, kPremul_SkAlphaType), 0,
      kTopLeft_GrSurfaceOrigin, nullptr);
  if (!g.opt) {
    std::fprintf(stderr, "acl-gpu: MakeRenderTarget fail\n");
    return false;
  }

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = g.qfam;
  if (VKD.vkCreateCommandPool(g.device, &pci, nullptr, &g.pool) != VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo cbi{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbi.commandPool = g.pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  if (VKD.vkAllocateCommandBuffers(g.device, &cbi, &g.cmd) != VK_SUCCESS)
    return false;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (VKD.vkCreateFence(g.device, &fci, nullptr, &g.fence) != VK_SUCCESS)
    return false;

  g.w = w;
  g.h = h;
  g.ready = true;
  return true;
}

// Copy the just-rendered optimal Skia image (TRANSFER_SRC_OPTIMAL) into `t`'s
// dma-buf image.
// Copy the flushed render target into `dst`, which is left in `final_layout`.
// The copy is made with `dst` in `copy_layout`. Waits for the GPU.
bool copy_opt_to(VkImage dst,
                 VkImageLayout copy_layout,
                 VkImageLayout final_layout) {
  GrBackendTexture bt =
      g.opt->getBackendTexture(SkSurface::kFlushRead_BackendHandleAccess);
  GrVkImageInfo si{};
  if (!bt.getVkImageInfo(&si)) {
    std::fprintf(stderr, "acl-gpu: getVkImageInfo fail\n");
    return false;
  }

  VKD.vkResetCommandBuffer(g.cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKD.vkBeginCommandBuffer(g.cmd, &bi);
  const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = copy_layout;
  toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.image = dst;
  toDst.subresourceRange = color;
  VKD.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &toDst);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {static_cast<uint32_t>(g.w), static_cast<uint32_t>(g.h), 1};
  VKD.vkCmdCopyImage(g.cmd, si.fImage,
                     static_cast<VkImageLayout>(si.fImageLayout), dst,
                     copy_layout, 1, &region);
  VkImageMemoryBarrier toExt{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toExt.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toExt.oldLayout = copy_layout;
  toExt.newLayout = final_layout;
  toExt.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toExt.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toExt.image = dst;
  toExt.subresourceRange = color;
  VKD.vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &toExt);
  VKD.vkEndCommandBuffer(g.cmd);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &g.cmd;
  if (VKD.vkQueueSubmit(g.queue, 1, &submit, g.fence) != VK_SUCCESS)
    return false;
  VKD.vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
  VKD.vkResetFences(g.device, 1, &g.fence);
  return true;
}

bool copy_to_dmabuf(Target& t) {
  return copy_opt_to(t.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}

void destroy_swapchain() {
  if (g.swapchain != VK_NULL_HANDLE)
    VKD.vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
  g.swapchain = VK_NULL_HANDLE;
  g.sc_images.clear();
  g.sc_w = g.sc_h = 0;
}

// MAILBOX where the surface has it, else FIFO, which every surface has. a4l
// paces frames itself, one per wl_surface.frame callback, and makes commits of
// its own on the same surface for that. A FIFO swapchain paces too: Mesa's
// holds each commit behind a wp_fifo_v1 barrier and a commit time, and with
// a4l's commits queued on the same surface a headless Weston stopped applying
// any of them after three frames. MAILBOX only attaches and commits.
VkPresentModeKHR pick_present_mode() {
  uint32_t n = 0;
  VKD.vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  VKD.vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n,
                                                modes.data());
  modes.resize(std::min<size_t>(modes.size(), n));
  const bool mailbox = std::find(modes.begin(), modes.end(),
                                 VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
  std::fprintf(stderr, "acl-gpu: swapchain present mode %s\n",
               mailbox ? "MAILBOX" : "FIFO");
  return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

// A swapchain of the window's size, replacing any older one.
bool make_swapchain(int w, int h) {
  VkSurfaceCapabilitiesKHR caps{};
  if (VKD.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps) !=
      VK_SUCCESS)
    return false;
  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
    std::fprintf(stderr, "acl-gpu: swapchain images cannot be copied to\n");
    return false;
  }
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0)
    count = std::min(count, caps.maxImageCount);
  // Premultiplied alpha where offered, as the dma-buf path presents (the
  // video hole is transparent); else whatever the surface does.
  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (VkCompositeAlphaFlagBitsKHR a :
       {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR}) {
    if ((caps.supportedCompositeAlpha & a) != 0) {
      alpha = a;
      break;
    }
  }
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = g.surface;
  ci.minImageCount = count;
  ci.imageFormat = g_fmt.vk;
  ci.imageColorSpace = g.sc_colorspace;
  ci.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = alpha;
  ci.presentMode = pick_present_mode();
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = g.swapchain;
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  const VkResult r = VKD.vkCreateSwapchainKHR(g.device, &ci, nullptr, &sc);
  destroy_swapchain();  // the old one is retired either way
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "acl-gpu: createSwapchain %dx%d fail (%d)\n", w, h,
                 static_cast<int>(r));
    return false;
  }
  uint32_t n = 0;
  VKD.vkGetSwapchainImagesKHR(g.device, sc, &n, nullptr);
  g.sc_images.resize(n);
  VKD.vkGetSwapchainImagesKHR(g.device, sc, &n, g.sc_images.data());
  g.sc_images.resize(std::min<size_t>(g.sc_images.size(), n));
  g.swapchain = sc;
  g.sc_w = w;
  g.sc_h = h;
  if (g.sc_fence == VK_NULL_HANDLE) {
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (VKD.vkCreateFence(g.device, &fci, nullptr, &g.sc_fence) != VK_SUCCESS)
      return false;
  }
  std::fprintf(
      stderr, "acl-gpu: swapchain %dx%d, %u images, format %d, alpha 0x%x\n", w,
      h, n, static_cast<int>(g_fmt.vk), static_cast<unsigned>(alpha));
  return !g.sc_images.empty();
}

// Copy the flushed render target into the next swapchain image and present
// it. The driver attaches the buffer to the wl_surface and commits it. False:
// the frame was not shown.
bool present_swapchain() {
  if ((g.swapchain == VK_NULL_HANDLE || g.sc_w != g.w || g.sc_h != g.h) &&
      !make_swapchain(g.w, g.h))
    return false;
  // Bounded: a compositor that has stopped taking frames must not hold the
  // render thread forever.
  constexpr uint64_t kAcquireNs = 1000ull * 1000 * 1000;
  uint32_t idx = 0;
  VkResult r = VKD.vkAcquireNextImageKHR(g.device, g.swapchain, kAcquireNs,
                                         VK_NULL_HANDLE, g.sc_fence, &idx);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    if (!make_swapchain(g.w, g.h))
      return false;
    r = VKD.vkAcquireNextImageKHR(g.device, g.swapchain, kAcquireNs,
                                  VK_NULL_HANDLE, g.sc_fence, &idx);
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    return false;  // a timeout included; the fence is untouched
  VKD.vkWaitForFences(g.device, 1, &g.sc_fence, VK_TRUE, UINT64_MAX);
  VKD.vkResetFences(g.device, 1, &g.sc_fence);
  if (idx >= g.sc_images.size())
    return false;
  if (!copy_opt_to(g.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR))
    return false;
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.swapchainCount = 1;
  pi.pSwapchains = &g.swapchain;
  pi.pImageIndices = &idx;
  r = VKD.vkQueuePresentKHR(g.queue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    destroy_swapchain();  // remade at the next frame
    return true;
  }
  return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

// Direct (zero-copy) path: wrap the modifier-tiled dma-buf VkImage itself as a
// Ganesh SkSurface so Skia renders STRAIGHT into the exported image — no
// optimal surface, no vkCmdCopyImage. Whether android-13 GrVkCaps accepts a
// DRM_FORMAT_MODIFIER image as a render target is the open question this path
// answers; falls back to the copy path if not.
bool make_direct_surface() {
  if (g.direct)
    return true;
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
  info.fFormat = g_fmt.vk;
  info.fImageUsageFlags = g.usage;
  info.fSampleCount = 1;
  info.fLevelCount = 1;
  info.fCurrentQueueFamily = g.qfam;
  info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  GrBackendTexture bt(g.w, g.h, info);
  g.direct = SkSurface::MakeFromBackendTexture(
      g.gr.get(), bt, kTopLeft_GrSurfaceOrigin, 1, g_fmt.ct, nullptr, nullptr);
  if (!g.direct) {
    std::fprintf(stderr,
                 "acl-gpu: MakeFromBackendTexture(direct tiled) fail\n");
    return false;
  }
  return true;
}

}  // namespace

// Render a scene GPU-native into a modifier-tiled dma-buf and export it; acl
// presents the fd.
extern "C" bool acl_graphics_gpu_render(int w,
                                        int h,
                                        uint32_t bg,
                                        int l,
                                        int t,
                                        int r,
                                        int b,
                                        uint32_t fg,
                                        AclDmabuf* out) {
  if (out == nullptr)
    return false;
  std::memset(out, 0, sizeof(*out));
  out->fd = -1;
  if (!ensure_ready(w, h))
    return false;
  if (!g.targets[0].allocated && !make_target(g.targets[0], w, h))
    return false;
  *out = g.targets[0].info;

  // Prefer the zero-copy direct path when ACL_GPU_DIRECT is set AND Skia
  // accepts the modifier-tiled image as a render target; otherwise use the
  // proven copy path.
  const bool want_direct = std::getenv("ACL_GPU_DIRECT") != nullptr;
  if (want_direct && make_direct_surface()) {
    SkCanvas* canvas = g.direct->getCanvas();
    canvas->clear(static_cast<SkColor>(bg));
    SkPaint paint;
    paint.setColor(static_cast<SkColor>(fg));
    paint.setAntiAlias(false);
    canvas->drawRect(SkRect::MakeLTRB(l, t, r, b), paint);
    // Hand the image to the compositor in GENERAL layout, released from our
    // queue family.
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

  GrBackendSurfaceMutableState toSrc(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     g.qfam);
  g.opt->flush(GrFlushInfo{}, &toSrc);
  g.gr->submit(/*syncCpu=*/true);
  if (!copy_to_dmabuf(g.targets[0]))
    return false;
  return out->fd >= 0;
}

extern "C" bool acl_graphics_gpu_available() {
  return true;
}

// ── S4b: drive the app's REAL android.graphics.Canvas onto the GPU surface
// ────────── begin_frame prepares the GPU render target and returns its
// SkCanvas as a handle so the runtime can wrap it in `new
// android.graphics.Canvas(long)`; the app then issues real Canvas ops
// (drawColor/drawRect/…) that land on the GPU surface. end_frame flushes Ganesh
// and vkCmdCopyImages the result into the modifier-tiled dma-buf (same copy
// path as the hardcoded renderer). The returned SkCanvas is owned by the
// SkSurface, so it is marked "borrowed" and graphics_jni's Canvas finalizer
// must NOT delete it.
namespace {
std::unordered_set<void*> g_borrowed_canvas;
}  // namespace

extern "C" bool acl_graphics_canvas_borrowed(void* c) {
  return g_borrowed_canvas.count(c) != 0;
}

extern "C" long acl_graphics_gpu_begin_frame(int w, int h) {
  if (!ensure_ready(w, h))
    return 0;
  SkCanvas* c = g.opt->getCanvas();
  c->clear(SK_ColorTRANSPARENT);
  g_borrowed_canvas.insert(c);
  return reinterpret_cast<long>(c);
}

// Flush the shared optimal surface and copy it into dma-buf image
// `buffer_index` (0..1), returning that buffer's descriptor. The animated loop
// alternates buffer_index so the compositor can hold one image while the next
// is rendered — no in-flight overwrite. DIAG: dump the just-flushed optimal
// surface to a PPM. Two modes (either/both):
//   ACL_GPU_DUMP=<file>      — single file overwritten each frame (most-recent
//   presented frame). ACL_GPU_DUMP_SEQ=<dir>   — a RING of complete per-frame
//   PPMs (frame-NN.ppm, NN = count % ring), so
//     consecutive frames survive to be compared — the ONLY way to capture a
//     per-frame FLICKER (a single overwritten file yields torn reads). Ring
//     size = ACL_GPU_DUMP_SEQ_N (default 30). Reconstruct the chronological
//     order by mtime (newest N files = the last N presents). PPM is
//     encoder-free.
static void maybe_dump_frame() {
  const char* path = std::getenv("ACL_GPU_DUMP");
  const char* seqdir = std::getenv("ACL_GPU_DUMP_SEQ");
  const bool single = path != nullptr && *path != '\0';
  const bool seq = seqdir != nullptr && *seqdir != '\0';
  if ((!single && !seq) || !g.opt)
    return;
  const int w = g.w, h = g.h;
  std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h);
  SkImageInfo info =
      SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
  if (!g.opt->readPixels(info, px.data(), static_cast<std::size_t>(w) * 4, 0,
                         0))
    return;
  // The PPM below is RGB-only, which hides the failure mode this dump is
  // usually chasing: the surface is cleared to SK_ColorTRANSPARENT, so a frame
  // with perfect color but ZERO alpha dumps as a flawless image and composites
  // as an invisible (blank) window. Report alpha separately (single mode only —
  // the seq ring runs every frame, so its alpha spam would be relentless).
  if (single) {
    std::size_t opaque = 0, clear = 0, partial = 0;
    for (std::uint32_t p : px) {
      const std::uint8_t a = static_cast<std::uint8_t>((p >> 24) & 0xff);
      if (a == 0)
        ++clear;
      else if (a == 0xff)
        ++opaque;
      else
        ++partial;
    }
    std::fprintf(
        stderr,
        "[acl.gpu] frame alpha: opaque=%zu clear=%zu partial=%zu (%dx%d)\n",
        opaque, clear, partial, w, h);
  }
  // Build the RGB PPM once into a buffer, then write it to whichever target(s)
  // are enabled — a whole-file write per frame, so each file is either the
  // previous complete frame or this complete frame (never a torn mix, unlike a
  // row-by-row write racing a reader).
  std::vector<std::uint8_t> buf;
  {
    char hdr[64];
    const int hn = std::snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", w, h);
    buf.reserve(static_cast<std::size_t>(hn) +
                static_cast<std::size_t>(w) * h * 3);
    buf.insert(buf.end(), hdr, hdr + hn);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const std::uint32_t p =
            px[static_cast<std::size_t>(y) * w + x];  // BGRA (little-endian)
        buf.push_back(static_cast<std::uint8_t>((p >> 16) & 0xff));  // R
        buf.push_back(static_cast<std::uint8_t>((p >> 8) & 0xff));   // G
        buf.push_back(static_cast<std::uint8_t>(p & 0xff));          // B
      }
    }
  }
  auto write_all = [&](const char* p) {
    FILE* f = std::fopen(p, "wb");
    if (f == nullptr)
      return;
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
  };
  if (single)
    write_all(path);
  if (seq) {
    static std::uint64_t count = 0;  // present thread is single-threaded here
    int ring = 30;
    if (const char* n = std::getenv("ACL_GPU_DUMP_SEQ_N")) {
      const int v = std::atoi(n);
      if (v > 0)
        ring = v;
    }
    char name[1024];
    std::snprintf(name, sizeof name, "%s/frame-%03llu.ppm", seqdir,
                  static_cast<unsigned long long>(
                      count % static_cast<std::uint64_t>(ring)));
    write_all(name);
    ++count;
  }
}

// Deferred-present replay: when the UI thread records the frame into an
// SkPicture (ACL_PRESENT_RT), replay it onto the GPU canvas here, just before
// flush. No-op in the default immediate path.
extern "C" void acl_graphics_present_replay_onto(void* sk_canvas);

extern "C" bool acl_graphics_gpu_end_frame(int buffer_index, AclDmabuf* out) {
  if (out == nullptr || !g.ready || !g.opt)
    return false;
  if (g_swapchain_mode) {
    acl_graphics_present_replay_onto(g.opt->getCanvas());
    GrBackendSurfaceMutableState toSrc(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       g.qfam);
    g.opt->flush(GrFlushInfo{}, &toSrc);
    g.gr->submit(/*syncCpu=*/true);
    maybe_dump_frame();
    if (!present_swapchain())
      return false;
    // No dma-buf: the frame is already on the surface.
    std::memset(out, 0, sizeof(*out));
    out->fd = -1;
    out->width = static_cast<uint32_t>(g.w);
    out->height = static_cast<uint32_t>(g.h);
    out->drm_format = g_fmt.drm;
    return true;
  }
  if (buffer_index < 0 || buffer_index >= kBuffers)
    buffer_index = 0;
  Target& t = g.targets[buffer_index];
  if (!t.allocated && !make_target(t, g.w, g.h))
    return false;
  acl_graphics_present_replay_onto(g.opt->getCanvas());
  GrBackendSurfaceMutableState toSrc(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     g.qfam);
  g.opt->flush(GrFlushInfo{}, &toSrc);
  g.gr->submit(/*syncCpu=*/true);
  maybe_dump_frame();
  if (!copy_to_dmabuf(t))
    return false;
  *out = t.info;
  return out->fd >= 0;
}

namespace {

// A SurfaceTexture frame's buffer imported into the present device: its
// VkImage and the memory imported from its dma-buf. The converter draws each
// frame into one of a few buffers it keeps, and a decoder decodes into one of
// a pool, so the import is made once a buffer and kept while frames keep
// arriving in it. The dma-buf, not the descriptor, is the buffer: each frame
// comes with a copy of the descriptor, so a buffer is known by the inode
// behind it, with its layout.
struct ImportKey {
  dev_t dev = 0;
  ino_t ino = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, plane_count = 0;
  uint32_t offset[2] = {0, 0}, stride[2] = {0, 0};
  uint64_t modifier = 0;
  bool operator==(const ImportKey& o) const {
    return dev == o.dev && ino == o.ino && format == o.format &&
           width == o.width && height == o.height &&
           plane_count == o.plane_count && offset[0] == o.offset[0] &&
           offset[1] == o.offset[1] && stride[0] == o.stride[0] &&
           stride[1] == o.stride[1] && modifier == o.modifier;
  }
};
struct Import {
  ImportKey key;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  VkImageUsageFlags usage = 0;
  int in_use = 0;  // Skia images made from it that are still alive
  std::chrono::steady_clock::time_point last;
};
// An import unused this long has gone with its buffer, or will be made again.
constexpr auto kImportIdle = std::chrono::seconds(1);
// More imports than this held at once are a sign of buffers being replaced
// faster than they go idle; the oldest unused ones go first. A decoder's pool
// is up to about 30 buffers.
constexpr std::size_t kMaxImports = 48;
std::mutex g_imports_mu;
std::vector<std::shared_ptr<Import>> g_imports;

void destroy_import(const Import& i) {
  VKD.vkDestroyImage(g.device, i.image, nullptr);
  VKD.vkFreeMemory(g.device, i.memory, nullptr);
}

// Let go of imports nothing uses that have been idle, and of the oldest
// unused ones while too many are held. Under g_imports_mu.
void prune_imports(std::chrono::steady_clock::time_point now) {
  auto idle = [&](const std::shared_ptr<Import>& i) {
    return i->in_use == 0 && now - i->last >= kImportIdle;
  };
  for (auto it = g_imports.begin(); it != g_imports.end();) {
    if (idle(*it)) {
      destroy_import(**it);
      it = g_imports.erase(it);
    } else {
      ++it;
    }
  }
  while (g_imports.size() > kMaxImports) {
    auto oldest = g_imports.end();
    for (auto it = g_imports.begin(); it != g_imports.end(); ++it) {
      if ((*it)->in_use == 0 &&
          (oldest == g_imports.end() || (*it)->last < (*oldest)->last))
        oldest = it;
    }
    if (oldest == g_imports.end())
      break;  // every one is in use
    destroy_import(**oldest);
    g_imports.erase(oldest);
  }
}

// Skia wraps only images made with both transfer bits.
constexpr VkImageUsageFlags kImportUsage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// Import a buffer: a VkImage of `key`'s format and planes over memory
// imported from its dma-buf. Null if the driver refuses.
std::shared_ptr<Import> make_import(const ImportKey& key, int fd) {
  VkSubresourceLayout planes[2]{};
  for (uint32_t i = 0; i < key.plane_count && i < 2; ++i) {
    planes[i].offset = key.offset[i];
    planes[i].rowPitch = key.stride[i];
  }
  VkImageDrmFormatModifierExplicitCreateInfoEXT modci{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  modci.drmFormatModifier = key.modifier;
  modci.drmFormatModifierPlaneCount = key.plane_count;
  modci.pPlaneLayouts = planes;
  VkExternalMemoryImageCreateInfo extci{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modci,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extci};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = key.format;
  ci.extent = {key.width, key.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ci.usage = kImportUsage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  if (VKD.vkCreateImage(g.device, &ci, nullptr, &image) != VK_SUCCESS)
    return nullptr;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  const auto fail = [&]() -> std::shared_ptr<Import> {
    if (memory != VK_NULL_HANDLE)
      VKD.vkFreeMemory(g.device, memory, nullptr);
    VKD.vkDestroyImage(g.device, image, nullptr);
    return nullptr;
  };
  // The image must be laid out as the buffer is. A driver may take a plane's
  // row pitch from the layouts given and place the plane itself (v3dv puts
  // the chroma plane after the luma rows the image has): a frame it would
  // read elsewhere than it is, it is not given.
  static const VkImageAspectFlagBits kMemoryPlane[2] = {
      VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
      VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT};
  for (uint32_t i = 0; i < key.plane_count && i < 2; ++i) {
    const VkImageSubresource sub{
        static_cast<VkImageAspectFlags>(kMemoryPlane[i]), 0, 0};
    VkSubresourceLayout lo{};
    VKD.vkGetImageSubresourceLayout(g.device, image, &sub, &lo);
    if (lo.offset != key.offset[i] || lo.rowPitch != key.stride[i])
      return fail();
  }
  // Vulkan owns the descriptor it imports, so it gets a copy of its own.
  const int own = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (own < 0)
    return fail();
  VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  VkMemoryRequirements reqs{};
  VKD.vkGetImageMemoryRequirements(g.device, image, &reqs);
  uint32_t types = reqs.memoryTypeBits;
  if (VKD.vkGetMemoryFdPropertiesKHR(
          g.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, own,
          &fdp) == VK_SUCCESS)
    types &= fdp.memoryTypeBits;
  if (types == 0) {
    ::close(own);
    return fail();
  }
  VkMemoryDedicatedAllocateInfo ded{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.image = image;
  VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded,
                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                              own};
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp,
                           reqs.size,
                           static_cast<uint32_t>(__builtin_ctz(types))};
  if (VKD.vkAllocateMemory(g.device, &mai, nullptr, &memory) != VK_SUCCESS) {
    ::close(own);  // not taken on failure
    return fail();
  }
  if (VKD.vkBindImageMemory(g.device, image, memory, 0) != VK_SUCCESS)
    return fail();
  auto i = std::make_shared<Import>();
  i->key = key;
  i->image = image;
  i->memory = memory;
  i->size = reqs.size;
  i->usage = ci.usage;
  return i;
}

// The import of `key`'s buffer, made on first use, with a Skia image counted
// against it; the caller wraps it. Null if the driver refuses.
std::shared_ptr<Import> take_import(const ImportKey& key, int fd) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(g_imports_mu);
  prune_imports(now);
  std::shared_ptr<Import> imp;
  for (const auto& i : g_imports) {
    if (i->key == key) {
      imp = i;
      break;
    }
  }
  if (!imp) {
    imp = make_import(key, fd);
    if (!imp)
      return nullptr;
    g_imports.push_back(imp);
  }
  ++imp->in_use;
  imp->last = now;
  return imp;
}

// `imp` as a texture image of `width` x `height`, read through `ycbcr` when
// it is valid. Written outside Vulkan, by the converter or the decoder, for
// every frame: each texture made from the image takes it from the external
// queue family, in a layout that keeps what is in it (acl_gpu_release_frame
// hands it back). `keep` goes when Skia has finished with the texture.
sk_sp<SkImage> wrap_import(GrDirectContext* ctx,
                           std::shared_ptr<Import> imp,
                           uint32_t width,
                           uint32_t height,
                           const GrVkYcbcrConversionInfo& ycbcr,
                           std::shared_ptr<void> keep) {
  GrVkImageInfo info{};
  info.fImage = imp->image;
  info.fAlloc.fMemory = imp->memory;
  info.fAlloc.fOffset = 0;
  info.fAlloc.fSize = imp->size;
  info.fImageTiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.fImageLayout = VK_IMAGE_LAYOUT_GENERAL;
  info.fFormat = imp->key.format;
  info.fImageUsageFlags = imp->usage;
  info.fSampleCount = 1;
  info.fLevelCount = 1;
  info.fCurrentQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;
  info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.fYcbcrConversionInfo = ycbcr;
  struct Held {
    std::shared_ptr<Import> imp;
    std::shared_ptr<void> keep;
  };
  auto* held = new Held{std::move(imp), std::move(keep)};
  const GrBackendTexture bt(static_cast<int>(width), static_cast<int>(height),
                            info);
  // A null image has already called the release proc.
  return SkImage::MakeFromTexture(
      ctx, bt, kTopLeft_GrSurfaceOrigin, kRGB_888x_SkColorType,
      kOpaque_SkAlphaType, nullptr,
      [](SkImage::ReleaseContext p) {
        auto* h = static_cast<Held*>(p);
        {
          std::lock_guard<std::mutex> lk(g_imports_mu);
          --h->imp->in_use;
          h->imp->last = std::chrono::steady_clock::now();
        }
        delete h;
      },
      held);
}

// What the device does with NV12 (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) laid
// out with `modifier`: 0 if it does not sample it, or cannot take it as a
// transfer source and destination too, which Skia asks of every image it
// wraps. Asked once a modifier. v3dv lists every modifier with one memory
// plane, NV12's too, and lays NV12 out in two all the same; make_import
// checks the layout it makes.
VkFormatFeatureFlags nv12_features(uint64_t modifier) {
  static std::mutex mu;
  static std::vector<std::pair<uint64_t, VkFormatFeatureFlags>> known;
  std::lock_guard<std::mutex> lk(mu);
  for (const auto& k : known) {
    if (k.first == modifier)
      return k.second;
  }
  VkDrmFormatModifierPropertiesListEXT list{
      VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
  VKD.vkGetPhysicalDeviceFormatProperties2(
      g.phys, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &props);
  std::vector<VkDrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  VKD.vkGetPhysicalDeviceFormatProperties2(
      g.phys, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &props);
  VkFormatFeatureFlags f = 0;
  for (uint32_t i = 0; i < list.drmFormatModifierCount && i < mods.size();
       ++i) {
    if (mods[i].drmFormatModifier == modifier &&
        (mods[i].drmFormatModifierPlaneCount == 2 ||
         mods[i].drmFormatModifierPlaneCount == 1))
      f = mods[i].drmFormatModifierTilingFeatures;
  }
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((f & need) != need ||
      (f & (VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
            VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)) == 0)
    f = 0;
  known.emplace_back(modifier, f);
  return f;
}

}  // namespace

// Whether the present device can draw a decoder's frame as it is
// (acl_gpu_import_yuv): it is up. It is made with the sampler YCbCr
// conversion, which Vulkan 1.1 has, and Skia is told so. Which layouts it
// takes is known only per frame.
bool acl_gpu_draws_yuv() {
  return g_draws_yuv.load(std::memory_order_acquire);
}

// Whether the present device samples a decoder's frame of this format and
// layout as it is (acl_gpu_import_yuv): NV12 in two planes, with a modifier
// it samples. The import may still refuse a frame (one in two buffers, a
// layout the driver places differently). Any thread.
bool acl_gpu_takes_yuv(uint32_t drm_format,
                       uint64_t modifier,
                       uint32_t plane_count) {
  return acl_gpu_draws_yuv() && drm_format == DRM_FORMAT_NV12 &&
         plane_count == 2 && nv12_features(modifier) != 0;
}

// A decoder's frame for a SurfaceTexture (graphics_jni.cpp), drawn by the
// present device with no copy: the frame's XBGR8888 dma-buf is imported as a
// VkImage, once a converter buffer, and wrapped as a texture image for each
// frame. `keep`, which holds the frame's buffer, goes when Skia has finished
// with that texture. Null when `ctx` is not the present context, the format
// is not one this takes, or the driver refuses the import; the caller then
// copies the pixels. Present thread.
sk_sp<SkImage> acl_gpu_import_frame(GrDirectContext* ctx,
                                    int fd,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t stride,
                                    uint32_t offset,
                                    uint32_t drm_format,
                                    uint64_t modifier,
                                    std::shared_ptr<void> keep) {
  if (ctx == nullptr || ctx != g.gr.get() || g.device == VK_NULL_HANDLE ||
      fd < 0 || width == 0 || height == 0)
    return nullptr;
  // Byte order R, G, B, X in memory either way; the fourth byte is unused.
  if (drm_format != DRM_FORMAT_XBGR8888 && drm_format != DRM_FORMAT_ABGR8888)
    return nullptr;
  struct stat st{};
  if (::fstat(fd, &st) != 0)
    return nullptr;
  ImportKey key;
  key.dev = st.st_dev;
  key.ino = st.st_ino;
  key.format = VK_FORMAT_R8G8B8A8_UNORM;
  key.width = width;
  key.height = height;
  key.plane_count = 1;
  key.offset[0] = offset;
  key.stride[0] = stride;
  key.modifier = modifier;
  std::shared_ptr<Import> imp = take_import(key, fd);
  if (!imp)
    return nullptr;
  return wrap_import(ctx, std::move(imp), width, height,
                     GrVkYcbcrConversionInfo{}, std::move(keep));
}

// A decoder's frame as it is, drawn by the present device: its NV12 dma-buf
// imported as a two-plane VkImage of the whole buffer and sampled through a
// YCbCr conversion with the frame's matrix and range. The caller draws the
// picture's crop of it. Null when the frame is not NV12 in one buffer, the
// device does not sample NV12 with the frame's modifier, or the driver
// refuses the import; the caller then has the frames converted. Present
// thread.
sk_sp<SkImage> acl_gpu_import_yuv(GrDirectContext* ctx,
                                  const AclStYuvFrame& y,
                                  std::shared_ptr<void> keep) {
  if (ctx == nullptr || ctx != g.gr.get() || g.device == VK_NULL_HANDLE ||
      y.drm_format != DRM_FORMAT_NV12 || y.plane_count != 2 || y.fd[0] < 0 ||
      y.width == 0 || y.height == 0)
    return nullptr;
  // One buffer: both planes behind the same inode.
  struct stat s0{}, s1{};
  if (::fstat(y.fd[0], &s0) != 0)
    return nullptr;
  if (y.fd[1] >= 0 && (::fstat(y.fd[1], &s1) != 0 || s1.st_dev != s0.st_dev ||
                       s1.st_ino != s0.st_ino))
    return nullptr;
  const VkFormatFeatureFlags features = nv12_features(y.modifier);
  if (features == 0)
    return nullptr;
  ImportKey key;
  key.dev = s0.st_dev;
  key.ino = s0.st_ino;
  key.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  key.width = y.crop_x + y.width;
  key.height = y.crop_y + y.height;
  // As many rows as the buffer has before its chroma plane: a decoder pads
  // the luma plane to its coded height (1088 for 1080 lines), and a driver
  // may place the chroma plane after the image's rows. The picture's crop is
  // what is drawn.
  if (y.stride[0] > 0 && y.offset[1] > y.offset[0] &&
      (y.offset[1] - y.offset[0]) % y.stride[0] == 0) {
    const uint32_t rows = (y.offset[1] - y.offset[0]) / y.stride[0];
    if (rows > key.height && rows <= key.height + 64)
      key.height = rows;
  }
  key.plane_count = 2;
  for (int i = 0; i < 2; ++i) {
    key.offset[i] = y.offset[i];
    key.stride[i] = y.stride[i];
  }
  key.modifier = y.modifier;
  GrVkYcbcrConversionInfo ycbcr;
  ycbcr.fFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  ycbcr.fYcbcrModel =
      y.yuv_matrix == 1   ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
      : y.yuv_matrix == 9 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020
                          : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
  ycbcr.fYcbcrRange = y.yuv_full_range != 0 ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                                            : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
  // H.264 and HEVC 4:2:0 put chroma level with the even luma columns and
  // between the rows, where the stream does not say otherwise.
  const bool cosited =
      (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT) != 0;
  const bool midpoint =
      (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) != 0;
  ycbcr.fXChromaOffset =
      cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT;
  ycbcr.fYChromaOffset =
      midpoint ? VK_CHROMA_LOCATION_MIDPOINT : VK_CHROMA_LOCATION_COSITED_EVEN;
  ycbcr.fChromaFilter =
      (features &
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0
          ? VK_FILTER_LINEAR
          : VK_FILTER_NEAREST;
  ycbcr.fForceExplicitReconstruction = VK_FALSE;
  ycbcr.fFormatFeatures = features;
  std::shared_ptr<Import> imp = take_import(key, y.fd[0]);
  if (!imp)
    return nullptr;
  return wrap_import(ctx, std::move(imp), key.width, key.height, ycbcr,
                     std::move(keep));
}

// Hand a frame's texture back to the external queue family, for the
// converter or the decoder to write the next frame into its buffer: an image
// acl_gpu_import_frame or acl_gpu_import_yuv made, which the caller is about
// to let go of. The transition goes with the next submit. Present thread.
void acl_gpu_release_frame(GrDirectContext* ctx, const sk_sp<SkImage>& img) {
  if (ctx == nullptr || ctx != g.gr.get() || !img || !img->isTextureBacked())
    return;
  const GrBackendTexture bt = img->getBackendTexture(false);
  if (!bt.isValid())
    return;
  ctx->setBackendTextureState(
      bt, GrBackendSurfaceMutableState(VK_IMAGE_LAYOUT_GENERAL,
                                       VK_QUEUE_FAMILY_EXTERNAL));
}

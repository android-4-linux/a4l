// SPDX-License-Identifier: Apache-2.0
// Provenance: acl P2.4/S4a de-risk; Skia public Ganesh Vulkan API
// (GrDirectContext:: MakeVulkan, GrVkBackendContext,
// SkSurface::MakeRenderTarget); Vulkan C API. No GPL-3.0 sources.
//
// Proves Android's (now Vulkan-enabled) host libskia renders via Ganesh Vulkan:
// create a Vulkan device, a Skia GrDirectContext(Vulkan), an offscreen GPU
// SkSurface, draw a rect, flush, and read the pixels back. If this builds +
// runs, the S4a foundation holds and the Canvas can render GPU-native into
// acl's dma-buf VkImage next.

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include <vulkan/vulkan.h>

#include "GrBackendSurface.h"
#include "GrDirectContext.h"
#include "SkCanvas.h"
#include "SkColor.h"
#include "SkImageInfo.h"
#include "SkPaint.h"
#include "SkRect.h"
#include "SkSurface.h"
#include "gpu/vk/GrVkBackendContext.h"
#include "gpu/vk/GrVkExtensions.h"
#include "gpu/vk/GrVkTypes.h"

int main() {
  // ── Load the system Vulkan loader (Soong can't -lvulkan on host; the real
  //    libacl_graphics_jni gets its entrypoints from acl instead) ────────────
  void* vklib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (vklib == nullptr)
    vklib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (vklib == nullptr) {
    std::printf("acl-ganesh-probe: no libvulkan\n");
    return 1;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(vklib, "vkGetInstanceProcAddr"));
  auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      dlsym(vklib, "vkGetDeviceProcAddr"));
  if (gipa == nullptr) {
    std::printf("acl-ganesh-probe: no vkGetInstanceProcAddr\n");
    return 1;
  }
#define VKI(name) reinterpret_cast<PFN_##name>(gipa(VK_NULL_HANDLE, #name))
#define VKID(inst, name) reinterpret_cast<PFN_##name>(gipa(inst, #name))
  auto vkCreateInstance = VKI(vkCreateInstance);

  // ── Vulkan device ────────────────────────────────────────────────────────
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "acl-ganesh-probe";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
    std::printf("acl-ganesh-probe: vkCreateInstance failed\n");
    return 1;
  }
  auto vkEnumeratePhysicalDevices = VKID(instance, vkEnumeratePhysicalDevices);
  auto vkGetPhysicalDeviceProperties =
      VKID(instance, vkGetPhysicalDeviceProperties);
  auto vkGetPhysicalDeviceQueueFamilyProperties =
      VKID(instance, vkGetPhysicalDeviceQueueFamilyProperties);
  auto vkCreateDevice = VKID(instance, vkCreateDevice);
  auto vkGetDeviceQueue = VKID(instance, vkGetDeviceQueue);

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance, &n, nullptr);
  if (n == 0) {
    std::printf("acl-ganesh-probe: no Vulkan device\n");
    return 1;
  }
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(instance, &n, devs.data());
  VkPhysicalDevice phys = devs[0];
  for (auto d : devs) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
        p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      phys = d;
      break;
    }
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys, &props);

  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qfs.data());
  uint32_t gfxq = UINT32_MAX;
  for (uint32_t i = 0; i < qn; ++i)
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      gfxq = i;
      break;
    }
  if (gfxq == UINT32_MAX) {
    std::printf("acl-ganesh-probe: no graphics queue\n");
    return 1;
  }

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = gfxq;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice device = VK_NULL_HANDLE;
  if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
    std::printf("acl-ganesh-probe: vkCreateDevice failed\n");
    return 1;
  }
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, gfxq, 0, &queue);

  // ── Skia Ganesh Vulkan context (all vk entrypoints via the loader) ────────
  GrVkGetProc getProc = [gipa, gdpa](const char* name, VkInstance inst,
                                     VkDevice dev) -> PFN_vkVoidFunction {
    if (dev != VK_NULL_HANDLE)
      return gdpa(dev, name);
    return gipa(inst, name);
  };
  GrVkExtensions ext;
  ext.init(getProc, instance, phys, 0, nullptr, 0, nullptr);

  GrVkBackendContext backend{};
  backend.fInstance = instance;
  backend.fPhysicalDevice = phys;
  backend.fDevice = device;
  backend.fQueue = queue;
  backend.fGraphicsQueueIndex = gfxq;
  backend.fMaxAPIVersion = VK_API_VERSION_1_1;
  backend.fVkExtensions = &ext;
  backend.fGetProc =
      getProc;  // fMemoryAllocator null -> Skia makes GrVkAMDMemoryAllocator

  sk_sp<GrDirectContext> ctx = GrDirectContext::MakeVulkan(backend);
  if (!ctx) {
    std::printf("acl-ganesh-probe: MakeVulkan failed\n");
    return 1;
  }

  // ── Offscreen GPU render + readback ──────────────────────────────────────
  const int W = 16, H = 10;
  SkImageInfo info =
      SkImageInfo::Make(W, H, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
  sk_sp<SkSurface> surface =
      SkSurface::MakeRenderTarget(ctx.get(), SkBudgeted::kYes, info);
  if (!surface) {
    std::printf("acl-ganesh-probe: MakeRenderTarget failed\n");
    return 1;
  }
  SkCanvas* canvas = surface->getCanvas();
  canvas->clear(SkColorSetARGB(0xFF, 0x20, 0x40, 0x60));
  SkPaint paint;
  paint.setColor(SkColorSetARGB(0xFF, 0xCC, 0x88, 0x44));
  paint.setAntiAlias(false);
  canvas->drawRect(SkRect::MakeLTRB(3, 2, 11, 7), paint);
  ctx->flushAndSubmit(/*syncCpu=*/true);

  std::vector<uint32_t> buf(static_cast<size_t>(W) * H);
  if (!surface->readPixels(info, buf.data(), W * sizeof(uint32_t), 0, 0)) {
    std::printf("acl-ganesh-probe: readPixels failed\n");
    return 1;
  }
  const uint32_t bg = buf[0];
  const uint32_t fg = buf[4 * W + 5];
  std::printf("acl-ganesh-probe: GPU %s bg(0,0)=0x%08x fg(5,4)=0x%08x\n",
              props.deviceName, bg, fg);
  return (bg == 0xFF204060u && fg == 0xFFCC8844u) ? 0 : 2;
}

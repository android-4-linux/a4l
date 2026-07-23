// SPDX-License-Identifier: Apache-2.0
// Provenance: struct/ABI mirror of AOSP android_webview/public/browser/draw_fn.h (Apache-2.0); the
// SkDrawable/GpuDrawHandler bridge is adapted from frameworks/base/libs/hwui/pipeline/skia/
// VkFunctorDrawable.cpp (Apache-2.0) — a custom SkDrawable that, on Skia's GPU flush, pulls the
// current secondary VkCommandBuffer + compatible render pass out of GrVkDrawableInfo and hands them
// to the Chromium WebView AwDrawFn Vulkan functor's draw_vk so Chromium records its compositing
// straight into acl's Ganesh-Vulkan present frame. No GPL-3.0 sources.
//
// The functor's init_vk/draw_vk/post_draw_vk callbacks live in the BIONIC plat_support lib; acl's
// side (src/ndk/webview_functor_vk.cpp) wraps them with a bionic-%fs RunScope. This file (glibc,
// libacl_graphics_jni) fills the draw_fn.h param structs from acl's VkDevice (gpu_dmabuf.cpp) and
// Skia's GrVkDrawableInfo, then calls those wrappers via function pointers acl registers at boot.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "vk/GrVkTypes.h"
#include "GrBackendDrawableInfo.h"
#include "SkCanvas.h"
#include "SkColorSpace.h"
#include "SkDrawable.h"
#include "SkM44.h"
#include "SkMatrix.h"
#include "SkRect.h"

// ── draw_fn.h ABI (Vulkan subset; overlay/SurfaceControl pointers kept opaque) ────────────────────
namespace {
constexpr int kAwDrawFnVersion = 3;
enum { AW_DRAW_FN_OVERLAYS_MODE_DISABLED = 0 };

struct AwDrawFn_InitVkParams {
  int version;
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t graphics_queue_index;
  uint32_t api_version;
  const char* const* enabled_instance_extension_names;
  uint32_t enabled_instance_extension_names_length;
  const char* const* enabled_device_extension_names;
  uint32_t enabled_device_extension_names_length;
  VkPhysicalDeviceFeatures* device_features;
  VkPhysicalDeviceFeatures2* device_features_2;
};

struct AwDrawFn_OnSyncParams {
  int version;
  bool apply_force_dark;
};

struct AwDrawFn_DrawVkParams {
  int version;
  int width;
  int height;
  bool deprecated_0;
  float transform[16];
  VkCommandBuffer secondary_command_buffer;
  uint32_t color_attachment_index;
  VkRenderPass compatible_render_pass;
  VkFormat format;
  float transfer_function_g;
  float transfer_function_a;
  float transfer_function_b;
  float transfer_function_c;
  float transfer_function_d;
  float transfer_function_e;
  float transfer_function_f;
  float color_space_toXYZD50[9];
  int clip_left;
  int clip_top;
  int clip_right;
  int clip_bottom;
  int overlays_mode;
  void* get_surface_control;
  void* merge_transaction;
};

struct AwDrawFn_PostDrawVkParams {
  int version;
};

// The acl bionic-%fs RunScope wrappers (src/ndk/webview_functor_vk.cpp), registered at boot by
// jvm_vm via acl_graphics_set_webview_vk_hooks. Null until then.
void (*g_on_sync)(int, void*) = nullptr;
void (*g_init_vk)(int, void*) = nullptr;
void (*g_draw_vk)(int, void*) = nullptr;
void (*g_post_draw_vk)(int, void*) = nullptr;
bool g_vk_inited = false;

// gpu_dmabuf.cpp: the present VkDevice handles for InitVk.
extern "C" bool acl_gpu_vk_handles(void** inst, void** phys, void** dev, void** q, uint32_t* qfam,
                                   const char* const** exts, uint32_t* n_exts, void** features2);

// Build + deliver InitVkParams once (Chromium DCHECKs a single InitVk per functor). Returns false if
// the device or the hooks aren't up yet (the WebView draw is then skipped this frame, not crashed).
bool ensure_init_vk(int functor) {
  if (g_vk_inited) return true;
  if (g_init_vk == nullptr) return false;
  void *inst = nullptr, *phys = nullptr, *dev = nullptr, *q = nullptr, *feat2 = nullptr;
  uint32_t qfam = 0, n_exts = 0;
  const char* const* exts = nullptr;
  if (!acl_gpu_vk_handles(&inst, &phys, &dev, &q, &qfam, &exts, &n_exts, &feat2)) return false;
  AwDrawFn_InitVkParams p{};
  p.version = kAwDrawFnVersion;
  p.instance = static_cast<VkInstance>(inst);
  p.physical_device = static_cast<VkPhysicalDevice>(phys);
  p.device = static_cast<VkDevice>(dev);
  p.queue = static_cast<VkQueue>(q);
  p.graphics_queue_index = qfam;
  p.api_version = VK_API_VERSION_1_1;
  p.enabled_instance_extension_names = nullptr;
  p.enabled_instance_extension_names_length = 0;
  p.enabled_device_extension_names = exts;
  p.enabled_device_extension_names_length = n_exts;
  p.device_features = nullptr;
  p.device_features_2 = static_cast<VkPhysicalDeviceFeatures2*>(feat2);
  g_init_vk(functor, &p);
  g_vk_inited = true;
  std::fprintf(stderr, "acl-webview: functor init_vk delivered (functor=%d)\n", functor);
  return true;
}

// Fill the color-space fields of DrawVkParams from an SkColorSpace (sRGB default).
void fill_color_space(const SkColorSpace* cs, AwDrawFn_DrawVkParams* p) {
  skcms_TransferFunction tf;
  skcms_Matrix3x3 m;
  if (cs != nullptr) {
    cs->transferFn(&tf);
    cs->toXYZD50(&m);
  } else {
    tf = SkNamedTransferFn::kSRGB;
    m = SkNamedGamut::kSRGB;
  }
  p->transfer_function_g = tf.g;
  p->transfer_function_a = tf.a;
  p->transfer_function_b = tf.b;
  p->transfer_function_c = tf.c;
  p->transfer_function_d = tf.d;
  p->transfer_function_e = tf.e;
  p->transfer_function_f = tf.f;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) p->color_space_toXYZD50[r * 3 + c] = m.vals[r][c];
}

// The GpuDrawHandler Skia invokes during flush: it hands us the frame's secondary command buffer +
// compatible render pass; we forward them to Chromium's draw_vk. Mirrors hwui VkFunctorDrawHandler.
class AclVkFunctorHandler : public SkDrawable::GpuDrawHandler {
 public:
  AclVkFunctorHandler(int functor, const SkMatrix& matrix, const SkIRect& clip,
                      const SkImageInfo& info)
      : functor_(functor), matrix_(matrix), clip_(clip), info_(info) {}

  ~AclVkFunctorHandler() override {
    if (drawn_ && g_post_draw_vk != nullptr) {
      AwDrawFn_PostDrawVkParams p{kAwDrawFnVersion};
      g_post_draw_vk(functor_, &p);
    }
  }

  void draw(const GrBackendDrawableInfo& info) override {
    GrVkDrawableInfo vk{};
    if (!info.getVkDrawableInfo(&vk)) return;
    if (!ensure_init_vk(functor_) || g_draw_vk == nullptr) return;

    // Sync BEFORE draw: AwDrawFnImpl::OnSync -> RenderThreadManager::CommitFrameOnRT pulls the produced
    // ChildFrame from the UI queue to the RenderThread queue that HardwareRenderer::Draw (inside
    // draw_vk) consumes. Without this the RT draws an empty child_frame_ -> blank web content. hwui
    // drove this via on_sync/draw_gl; the Vulkan present path must call it here, on the RT, per frame.
    if (g_on_sync != nullptr) {
      AwDrawFn_OnSyncParams sp{kAwDrawFnVersion, /*apply_force_dark=*/false};
      g_on_sync(functor_, &sp);
    }

    AwDrawFn_DrawVkParams p{};
    p.version = kAwDrawFnVersion;
    p.width = info_.width();
    p.height = info_.height();
    p.deprecated_0 = false;
    SkM44(matrix_).getColMajor(&p.transform[0]);
    p.secondary_command_buffer = vk.fSecondaryCommandBuffer;
    p.color_attachment_index = vk.fColorAttachmentIndex;
    p.compatible_render_pass = vk.fCompatibleRenderPass;
    p.format = vk.fFormat;
    fill_color_space(info_.colorSpace(), &p);
    p.clip_left = clip_.fLeft;
    p.clip_top = clip_.fTop;
    p.clip_right = clip_.fRight;
    p.clip_bottom = clip_.fBottom;
    p.overlays_mode = AW_DRAW_FN_OVERLAYS_MODE_DISABLED;
    p.get_surface_control = nullptr;
    p.merge_transaction = nullptr;

    g_draw_vk(functor_, &p);
    drawn_ = true;

    // Tell Skia the region the functor drew (its clip), so it tracks the damage.
    if (vk.fDrawBounds != nullptr) {
      vk.fDrawBounds->offset.x = clip_.fLeft;
      vk.fDrawBounds->offset.y = clip_.fTop;
      vk.fDrawBounds->extent.width = static_cast<uint32_t>(clip_.width());
      vk.fDrawBounds->extent.height = static_cast<uint32_t>(clip_.height());
    }
  }

 private:
  int functor_;
  SkMatrix matrix_;
  SkIRect clip_;
  SkImageInfo info_;
  bool drawn_ = false;
};

// The SkDrawable acl draws onto the present canvas for a WebView functor.
class AclVkFunctorDrawable : public SkDrawable {
 public:
  AclVkFunctorDrawable(int functor, const SkRect& bounds) : functor_(functor), bounds_(bounds) {}

 protected:
  SkRect onGetBounds() override { return bounds_; }
  void onDraw(SkCanvas* canvas) override {
    // Only reached for non-GPU (picture/software) canvases — nothing to composite there.
    (void)canvas;
  }
  std::unique_ptr<GpuDrawHandler> onSnapGpuDrawHandler(GrBackendApi api, const SkMatrix& matrix,
                                                       const SkIRect& clip,
                                                       const SkImageInfo& info) override {
    if (api != GrBackendApi::kVulkan) return nullptr;
    return std::make_unique<AclVkFunctorHandler>(functor_, matrix, clip, info);
  }

 private:
  int functor_;
  SkRect bounds_;
};

}  // namespace

// Registered at boot by jvm_vm (which holds the acl-side RunScope wrappers). Wiring acl->graphics_jni
// like acl_graphics_set_present_canvas.
extern "C" void acl_graphics_set_webview_vk_hooks(void (*on_sync)(int, void*),
                                                  void (*init_vk)(int, void*),
                                                  void (*draw_vk)(int, void*),
                                                  void (*post_draw_vk)(int, void*)) {
  g_on_sync = on_sync;
  g_init_vk = init_vk;
  g_draw_vk = draw_vk;
  g_post_draw_vk = post_draw_vk;
  std::fprintf(stderr, "acl-webview: VK functor hooks registered\n");
}

// Called by RecordingCanvas_nDrawWebViewFunctor: draw the WebView functor onto `canvas`. Skia routes
// this through onSnapGpuDrawHandler at flush time. Returns false if the VK seam isn't wired (caller
// then leaves the WebView area unpainted rather than crashing).
extern "C" bool acl_graphics_draw_webview_functor(void* sk_canvas, int functor) {
  // Only drive the VK functor when Chromium is actually in Vulkan render mode (ACL_WEBVIEW_VULKAN,
  // the same gate plat_support's query_render_mode reads). In GL mode the functor's draw_vk/init_vk
  // must NOT be called — init_vk would spin up Chromium's Vulkan context (and hit the loader), so
  // leave the WebView area unpainted there rather than crash the stable GL/software path.
  static const bool vk_mode = std::getenv("ACL_WEBVIEW_VULKAN") != nullptr;
  if (!vk_mode || g_draw_vk == nullptr || sk_canvas == nullptr) return false;
  auto* canvas = reinterpret_cast<SkCanvas*>(sk_canvas);
  SkIRect clip = canvas->getDeviceClipBounds();
  if (clip.isEmpty()) clip = SkIRect::MakeWH(canvas->imageInfo().width(), canvas->imageInfo().height());
  sk_sp<SkDrawable> drawable(new AclVkFunctorDrawable(functor, SkRect::Make(clip)));
  canvas->drawDrawable(drawable.get());
  return true;
}

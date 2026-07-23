// SPDX-License-Identifier: Apache-2.0
// Provenance: struct/ABI mirror of AOSP android_webview/public/browser/draw_fn.h +
// frameworks/base/native/webview/plat_support (draw_functor/graphics_utils, Apache-2.0). No GPL-3.0 sources.
//
// Staged as bionic libwebviewchromium_plat_support.so — the framework draw-functor glue the Chromium
// WebView provider loads (System.loadLibrary). On a device this bridges WebView's render to libhwui's
// RenderThread WebViewFunctor; acl dropped hwui, so acl HOSTS the functor itself. This lib:
//   - returns a REAL AwDrawFnFunctionTable from DrawFunctor.nativeGetFunctionTable (GL render mode),
//     so Chromium has a valid render path (a null table -> NOTREACHED during GPU init);
//   - create_functor_v3 stores Chromium's callbacks in a small registry keyed by an int handle;
//   - exports acl_webview_functor_* so acl's graphics/present (glibc side, via the bionic %fs donor)
//     can drive the stored callbacks: on_sync then draw_gl(DrawGLParams) with a live GLES context,
//     so Chromium rasters web content into acl's framebuffer (the render bridge).
// The legacy DrawGLFunctor / GraphicsUtils tables stay null — modern WebView (ForT) uses AwDrawFn.
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ── draw_fn.h ABI (GL subset; SurfaceControl/Vulkan pointers kept opaque) ─────────────────────────
enum { AW_DRAW_FN_RENDER_MODE_OPENGL_ES = 0, AW_DRAW_FN_RENDER_MODE_VULKAN = 1 };
static const int kAwDrawFnVersion = 3;

typedef void (*AwDrawFn_OnSync)(int functor, void* data, void* params);              // AwDrawFn_OnSyncParams*
typedef void (*AwDrawFn_OnContextDestroyed)(int functor, void* data);
typedef void (*AwDrawFn_OnDestroyed)(int functor, void* data);
typedef void (*AwDrawFn_DrawGL)(int functor, void* data, void* params);              // AwDrawFn_DrawGLParams*
typedef void (*AwDrawFn_InitVk)(int functor, void* data, void* params);
typedef void (*AwDrawFn_DrawVk)(int functor, void* data, void* params);
typedef void (*AwDrawFn_PostDrawVk)(int functor, void* data, void* params);
typedef void (*AwDrawFn_RemoveOverlays)(int functor, void* data, void* params);

typedef struct {
  AwDrawFn_OnSync on_sync;
  AwDrawFn_OnContextDestroyed on_context_destroyed;
  AwDrawFn_OnDestroyed on_destroyed;
  AwDrawFn_DrawGL draw_gl;
  AwDrawFn_InitVk init_vk;
  AwDrawFn_DrawVk draw_vk;
  AwDrawFn_PostDrawVk post_draw_vk;
  AwDrawFn_RemoveOverlays remove_overlays;  // v3
} AwDrawFnFunctorCallbacks;

typedef int  (*AwDrawFn_QueryRenderMode)(void);
typedef int  (*AwDrawFn_CreateFunctor)(void* data, AwDrawFnFunctorCallbacks* cb);
typedef void (*AwDrawFn_ReleaseFunctor)(int functor);
typedef int  (*AwDrawFn_CreateFunctor_v3)(void* data, int version, AwDrawFnFunctorCallbacks* cb);

typedef struct {
  int version;
  AwDrawFn_QueryRenderMode query_render_mode;
  AwDrawFn_CreateFunctor create_functor;      // <= v3
  AwDrawFn_ReleaseFunctor release_functor;
  AwDrawFn_CreateFunctor_v3 create_functor_v3;  // v3
} AwDrawFnFunctionTable;

// ── functor registry ──────────────────────────────────────────────────────────────────────────
#define ACL_MAX_FUNCTORS 64
static struct { int used; int released; void* data; AwDrawFnFunctorCallbacks cb; } g_functors[ACL_MAX_FUNCTORS];

// The render mode is THE single gate that flips Chromium's whole VK-vs-GL path: when it returns
// VULKAN, AwDrawFnImpl::IsUsingVulkan() is true, aw_main_delegate appends
// --webview-draw-functor-uses-vulkan, and the functor's init_vk/draw_vk/post_draw_vk callbacks are
// invoked instead of draw_gl (non-interop, pure-Vulkan — acl's Ganesh-Vulkan present model). acl gates
// it on ACL_WEBVIEW_VULKAN so the mode can be turned on only once acl's VK functor seam + present
// device are ready (returning VULKAN before then would make Chromium's mandatory gpu InitializeVulkan
// CHECK-crash, no fallback). Read once (Chromium queries it early, before the first draw).
static int query_render_mode(void) {
  static int mode = -1;
  if (mode < 0)
    mode = (getenv("ACL_WEBVIEW_VULKAN") != NULL) ? AW_DRAW_FN_RENDER_MODE_VULKAN
                                                  : AW_DRAW_FN_RENDER_MODE_OPENGL_ES;
  return mode;
}

static int create_functor_v3(void* data, int version, AwDrawFnFunctorCallbacks* cb) {
  (void)version;
  for (int i = 1; i < ACL_MAX_FUNCTORS; ++i) {
    if (!g_functors[i].used) {
      g_functors[i].used = 1;
      g_functors[i].released = 0;
      g_functors[i].data = data;
      g_functors[i].cb = *cb;
      return i;
    }
  }
  return -1;
}
static int create_functor(void* data, AwDrawFnFunctorCallbacks* cb) {
  return create_functor_v3(data, 1, cb);
}
// May be called on any thread; the functor stays alive until its last use is released. acl calls
// on_destroyed from acl_webview_functor_destroy (with a context current); here just mark released.
static void release_functor(int functor) {
  if (functor > 0 && functor < ACL_MAX_FUNCTORS) g_functors[functor].released = 1;
}

static AwDrawFnFunctionTable g_table = {
    3 /*version*/, query_render_mode, create_functor, release_functor, create_functor_v3};

// ── acl-side seam (called from acl graphics/present under a bionic %fs donor) ─────────────────────
// Render mode acl should drive (GL). draw_gl invokes Chromium's raster into the CURRENT GLES
// framebuffer; on_sync runs while the UI thread is blocked (state upload). params point at the
// ABI structs in draw_fn.h (acl fills AwDrawFn_DrawGLParams). Safe no-ops for unknown/released handles.
__attribute__((visibility("default"))) int acl_webview_functor_render_mode(void) { return query_render_mode(); }

__attribute__((visibility("default"))) void acl_webview_functor_on_sync(int functor, void* on_sync_params) {
  if (functor > 0 && functor < ACL_MAX_FUNCTORS && g_functors[functor].used && g_functors[functor].cb.on_sync)
    g_functors[functor].cb.on_sync(functor, g_functors[functor].data, on_sync_params);
}
__attribute__((visibility("default"))) void acl_webview_functor_draw_gl(int functor, void* draw_gl_params) {
  if (functor > 0 && functor < ACL_MAX_FUNCTORS && g_functors[functor].used && g_functors[functor].cb.draw_gl)
    g_functors[functor].cb.draw_gl(functor, g_functors[functor].data, draw_gl_params);
}
// Vulkan seam (RENDER_MODE_VULKAN): acl's Ganesh-Vulkan present drives these instead of on_sync/draw_gl.
// init_vk (once) hands Chromium acl's VkInstance/PhysicalDevice/Device/Queue via AwDrawFn_InitVkParams;
// draw_vk records Chromium's compositing into the AwDrawFn_DrawVkParams.secondary_command_buffer (which
// acl fills from Skia's GrVkDrawableInfo for the present frame); post_draw_vk finalizes. Called under a
// bionic %fs (RunScope) — no GL, so no GL donor. Safe no-ops for unknown/released handles.
__attribute__((visibility("default"))) void acl_webview_functor_init_vk(int functor, void* init_vk_params) {
  if (functor > 0 && functor < ACL_MAX_FUNCTORS && g_functors[functor].used && g_functors[functor].cb.init_vk)
    g_functors[functor].cb.init_vk(functor, g_functors[functor].data, init_vk_params);
}
__attribute__((visibility("default"))) void acl_webview_functor_draw_vk(int functor, void* draw_vk_params) {
  if (functor > 0 && functor < ACL_MAX_FUNCTORS && g_functors[functor].used && g_functors[functor].cb.draw_vk)
    g_functors[functor].cb.draw_vk(functor, g_functors[functor].data, draw_vk_params);
}
__attribute__((visibility("default"))) void acl_webview_functor_post_draw_vk(int functor, void* post_draw_vk_params) {
  if (functor > 0 && functor < ACL_MAX_FUNCTORS && g_functors[functor].used && g_functors[functor].cb.post_draw_vk)
    g_functors[functor].cb.post_draw_vk(functor, g_functors[functor].data, post_draw_vk_params);
}
// Called by acl with a GLES context CURRENT when a functor is being torn down (release_functor was
// signalled): deliver on_context_destroyed + on_destroyed, then free the slot.
__attribute__((visibility("default"))) void acl_webview_functor_destroy(int functor) {
  if (functor <= 0 || functor >= ACL_MAX_FUNCTORS || !g_functors[functor].used) return;
  if (g_functors[functor].cb.on_context_destroyed)
    g_functors[functor].cb.on_context_destroyed(functor, g_functors[functor].data);
  if (g_functors[functor].cb.on_destroyed)
    g_functors[functor].cb.on_destroyed(functor, g_functors[functor].data);
  memset(&g_functors[functor], 0, sizeof(g_functors[functor]));
}

// ── present-timing relay (WebView presentation feedback) ─────────────────────────────────────────
// acl's present path calls set_present_timing each frame with the host compositor's real
// wp_presentation feedback (present-clock ns + refresh interval ns + WP_PRESENTATION_FEEDBACK_KIND_*
// flags). Chromium's Viz output device (skia_output_device.cc CallFeedback) dlsym's
// acl_webview_get_present_timing from THIS lib (same bionic namespace) and reports it, so WebView
// pacing / presentation-time callbacks get true vsync timing instead of the draw-time
// TimeTicks::Now() + zero interval. Stored here (not in the engine) so no engine export is needed —
// libwebviewchromium.so's link strips/hides non-whitelisted symbols, but plat_support exports freely.
// Cross-thread (acl's RenderThread writes, the Viz thread reads); C11 atomics keep it tear-free.
static _Atomic int64_t g_pt_present_ns;
static _Atomic int64_t g_pt_refresh_ns;
static _Atomic uint32_t g_pt_flags;

__attribute__((visibility("default"))) void acl_webview_functor_set_present_timing(
    int64_t present_ns, int64_t refresh_ns, uint32_t flags) {
  atomic_store_explicit(&g_pt_present_ns, present_ns, memory_order_relaxed);
  atomic_store_explicit(&g_pt_refresh_ns, refresh_ns, memory_order_relaxed);
  atomic_store_explicit(&g_pt_flags, flags, memory_order_relaxed);
}

// Chromium reads this in CallFeedback. Returns 1 with the last feedback once a frame has been
// presented (present_ns > 0), else 0 (Chromium then keeps its draw-time fallback).
__attribute__((visibility("default"))) int acl_webview_get_present_timing(int64_t* present_ns,
                                                                          int64_t* refresh_ns,
                                                                          uint32_t* flags) {
  int64_t p = atomic_load_explicit(&g_pt_present_ns, memory_order_relaxed);
  {
  }
  if (p <= 0) return 0;
  if (present_ns != NULL) *present_ns = p;
  if (refresh_ns != NULL) *refresh_ns = atomic_load_explicit(&g_pt_refresh_ns, memory_order_relaxed);
  if (flags != NULL) *flags = atomic_load_explicit(&g_pt_flags, memory_order_relaxed);
  return 1;
}

// ── JNI: register the three plat_support native groups ────────────────────────────────────────
static jlong drawfunctor_getFunctionTable(JNIEnv* e, jclass c) { (void)e; (void)c; return (jlong)(intptr_t)&g_table; }
static jlong drawgl_create(JNIEnv* e, jclass c, jlong h) { (void)e; (void)c; (void)h; return 0; }
static void drawgl_destroy(JNIEnv* e, jclass c, jlong h) { (void)e; (void)c; (void)h; }
static void drawgl_setChromiumFn(JNIEnv* e, jclass c, jlong h) { (void)e; (void)c; (void)h; }
static jlong graphics_swTable(JNIEnv* e, jclass c) { (void)e; (void)c; return 0; }  // legacy; unused (AwDrawFn)
static jlong graphics_glTable(JNIEnv* e, jclass c) { (void)e; (void)c; return 0; }  // legacy; unused (AwDrawFn)

static void reg(JNIEnv* env, const char* cls, const JNINativeMethod* m, int n) {
  jclass c = (*env)->FindClass(env, cls);
  if (c != 0) (*env)->RegisterNatives(env, c, m, n);
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env = 0;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == 0)
    return JNI_VERSION_1_6;
  static const JNINativeMethod kDrawFunctor[] = {
      {"nativeGetFunctionTable", "()J", (void*)drawfunctor_getFunctionTable}};
  static const JNINativeMethod kDrawGLFunctor[] = {
      {"nativeCreateGLFunctor", "(J)J", (void*)drawgl_create},
      {"nativeDestroyGLFunctor", "(J)V", (void*)drawgl_destroy},
      {"nativeSetChromiumAwDrawGLFunction", "(J)V", (void*)drawgl_setChromiumFn}};
  static const JNINativeMethod kGraphicsUtils[] = {
      {"nativeGetDrawSWFunctionTable", "()J", (void*)graphics_swTable},
      {"nativeGetDrawGLFunctionTable", "()J", (void*)graphics_glTable}};
  reg(env, "com/android/webview/chromium/DrawFunctor", kDrawFunctor, 1);
  reg(env, "com/android/webview/chromium/DrawGLFunctor", kDrawGLFunctor, 3);
  reg(env, "com/android/webview/chromium/GraphicsUtils", kGraphicsUtils, 2);
  return JNI_VERSION_1_6;
}

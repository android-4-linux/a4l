// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl seam (mirrors the RunScope pattern in ndk_probe_main.cpp). Drives the
// Chromium WebView AwDrawFn Vulkan functor callbacks (bionic, in libwebviewchromium_plat_support.so)
// from acl's glibc-side Ganesh-Vulkan present. No GPL-3.0 sources.

#include "ndk/webview_functor_vk.h"

#include <cstdlib>
#include <mutex>
#include <string>

#include "log/log.h"
#include "ndk/bionic_runtime.h"

namespace {
constexpr const char* kTag = "acl.webview.vk";

void (*g_on_sync)(int, void*) = nullptr;
void (*g_init_vk)(int, void*) = nullptr;
void (*g_draw_vk)(int, void*) = nullptr;
void (*g_post_draw_vk)(int, void*) = nullptr;
bool g_ok = false;
std::once_flag g_once;

// Resolve the three functor VK seam exports from the bionic plat_support lib (already loaded by
// Chromium's System.loadLibrary; rt->load is idempotent on the soname, so this just hands back the
// live handle to symbol()).
void resolve() {
  auto* rt = acl::ndk::BionicRuntime::get();
  if (rt == nullptr) { acl::log::w(kTag, "no bionic runtime; WebView VK functor disabled"); return; }
  const char* sysroot = std::getenv("ACL_BIONIC_SYSROOT");
  if (sysroot == nullptr) { acl::log::w(kTag, "no ACL_BIONIC_SYSROOT"); return; }
  std::string err;
  void* h = rt->load((std::string(sysroot) + "/libwebviewchromium_plat_support.so").c_str(), &err);
  if (h == nullptr) { acl::log::w(kTag, "load plat_support failed: {}", err); return; }
  g_on_sync = reinterpret_cast<void (*)(int, void*)>(rt->symbol(h, "acl_webview_functor_on_sync"));
  g_init_vk = reinterpret_cast<void (*)(int, void*)>(rt->symbol(h, "acl_webview_functor_init_vk"));
  g_draw_vk = reinterpret_cast<void (*)(int, void*)>(rt->symbol(h, "acl_webview_functor_draw_vk"));
  g_post_draw_vk = reinterpret_cast<void (*)(int, void*)>(rt->symbol(h, "acl_webview_functor_post_draw_vk"));
  g_ok = g_init_vk != nullptr && g_draw_vk != nullptr && g_post_draw_vk != nullptr;
  if (g_ok) acl::log::i(kTag, "WebView VK functor seam resolved");
  else acl::log::w(kTag, "WebView VK functor exports missing");
}

// Call a resolved bionic functor callback under this thread's own bionic %fs. The caller (graphics_jni
// present/flush thread) is a glibc thread; attach it to bionic once so RunScope has a TCB to swap to.
void call_bionic(void (*fn)(int, void*), int functor, void* params) {
  auto* rt = acl::ndk::BionicRuntime::get();
  if (rt == nullptr || fn == nullptr) return;
  if (acl::ndk::BionicRuntime::current_thread_fs() == 0) {
    std::string why;
    if (rt->attach_current_thread(&why) == 0) { acl::log::w(kTag, "attach failed: {}", why); return; }
  }
  acl::ndk::BionicRuntime::RunScope scope(*rt);
  fn(functor, params);
}
}  // namespace

extern "C" {

int acl_webview_functor_vk_ready() {
  std::call_once(g_once, resolve);
  return g_ok ? 1 : 0;
}

void acl_webview_functor_vk_sync(int functor, void* on_sync_params) {
  std::call_once(g_once, resolve);
  call_bionic(g_on_sync, functor, on_sync_params);
}

void acl_webview_functor_vk_init(int functor, void* init_vk_params) {
  std::call_once(g_once, resolve);
  call_bionic(g_init_vk, functor, init_vk_params);
}

void acl_webview_functor_vk_draw(int functor, void* draw_vk_params) {
  std::call_once(g_once, resolve);
  call_bionic(g_draw_vk, functor, draw_vk_params);
}

void acl_webview_functor_vk_post_draw(int functor, void* post_draw_vk_params) {
  std::call_once(g_once, resolve);
  call_bionic(g_post_draw_vk, functor, post_draw_vk_params);
}

}  // extern "C"

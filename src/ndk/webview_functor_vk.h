// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl seam. The %fs bridge that lets acl's Ganesh-Vulkan present (glibc side)
// drive the Chromium WebView AwDrawFn Vulkan functor, whose init_vk/draw_vk/post_draw_vk callbacks
// live in the bionic libwebviewchromium_plat_support.so. No GPL-3.0 sources.
#ifndef ACL_NDK_WEBVIEW_FUNCTOR_VK_H
#define ACL_NDK_WEBVIEW_FUNCTOR_VK_H

// These run the bionic functor callbacks under the calling thread's bionic %fs (BionicRuntime::
// RunScope), attaching the thread on first use. `params` is an opaque pointer to the caller-owned
// AwDrawFn_{InitVk,DrawVk,PostDrawVk}Params struct (graphics_jni owns the draw_fn.h layout and fills
// it from acl's VkDevice + Skia's GrVkDrawableInfo). No GL is involved (pure Vulkan), so no GL donor.
extern "C" {
// Returns 1 once the bionic functor VK exports are resolved (the plat_support lib is loaded), else 0.
int acl_webview_functor_vk_ready();
// on_sync drives Chromium's AwDrawFnImpl::OnSync -> RenderThreadManager::CommitFrameOnRT, the per-frame
// pull that moves the produced ChildFrame from the UI-side queue to the RenderThread queue that
// HardwareRenderer::Draw consumes. MUST run (on the RT) before draw_vk each frame, or the WebView draws
// an empty child_frame_ (blank content). The GL path got this via hwui's on_sync/draw_gl; the Vulkan
// present path must call it explicitly.
void acl_webview_functor_vk_sync(int functor, void* on_sync_params);
void acl_webview_functor_vk_init(int functor, void* init_vk_params);
void acl_webview_functor_vk_draw(int functor, void* draw_vk_params);
void acl_webview_functor_vk_post_draw(int functor, void* post_draw_vk_params);
}

#endif  // ACL_NDK_WEBVIEW_FUNCTOR_VK_H

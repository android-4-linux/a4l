// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (the Vulkan analog of tools/gen-gl-forwarders.py's libEGL/libGLESv2
// forwarders; see docs/ndk-scope.md). No GPL-3.0 sources.
//
// Staged as bionic libvulkan.so. Chromium (bionic) loads this via base::LoadNativeLibrary and dlsyms
// vkGetInstanceProcAddr; it resolves every other Vulkan entrypoint THROUGH that. acl's own present
// (graphics_jni, glibc) creates its VkInstance/device with the HOST (glibc) Mesa libvulkan, so for
// Chromium's handles to be recognized it must use the SAME host loader. This forwarder's
// vkGetInstanceProcAddr resolves THIS thread's glibc donor %fs (Mesa's TLS home) via acl_gl_pick_donor
// (shared with the GL forwarders; DT_NEEDED libEGL.so), %fs-swaps to it, and calls acl's glibc-side
// acl_vk_get_instance_proc_addr (bridge fns[0]) — which forwards to host Mesa and hands back each
// returned pointer WRAPPED in a per-thread-donor %fs trampoline (like acl_egl_get_proc_address), so the
// engine can call Vulkan directly from bionic %fs. acl writes the bridge pointer after this lib loads.
#include <stdint.h>

uintptr_t acl_gl_pick_donor(void);
// { uintptr_t donor_fs; void* fns[]; } — fns[0] = acl_vk_get_instance_proc_addr. Set by acl
// (src/ndk/bionic_runtime.cpp install_gl_forwarders).
void* acl_vk_bridge = 0;

// vkGetInstanceProcAddr(VkInstance, const char*) — 2 register args; the 8-slot stack copy (same as the
// generated GL forwarders) is harmless here (no stack args) and keeps the frame/alignment identical.
asm(
    ".text\n"
    ".globl vkGetInstanceProcAddr\n"
    "vkGetInstanceProcAddr:\n"
    "  push %rbp\n"
    "  mov %rsp, %rbp\n"
    "  call acl_gl_pick_donor@PLT\n"
    "  rdfsbase %r11\n"
    "  mov %rax, %r10\n"
    "  push %r11\n"
    "  push %r10\n"
    "  mov 72(%rbp), %rax\n  push %rax\n"
    "  mov 64(%rbp), %rax\n  push %rax\n"
    "  mov 56(%rbp), %rax\n  push %rax\n"
    "  mov 48(%rbp), %rax\n  push %rax\n"
    "  mov 40(%rbp), %rax\n  push %rax\n"
    "  mov 32(%rbp), %rax\n  push %rax\n"
    "  mov 24(%rbp), %rax\n  push %rax\n"
    "  mov 16(%rbp), %rax\n  push %rax\n"
    "  wrfsbase %r10\n"
    "  mov acl_vk_bridge@GOTPCREL(%rip), %r10\n"
    "  mov (%r10), %r10\n"
    "  mov 8(%r10), %r10\n"          // fns[0] = acl_vk_get_instance_proc_addr
    "  call *%r10\n"
    "  mov -8(%rbp), %r11\n"
    "  wrfsbase %r11\n"
    "  mov %rbp, %rsp\n"
    "  pop %rbp\n"
    "  ret\n");

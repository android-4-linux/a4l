#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling. Symbol names are the standard Khronos EGL 1.4 / GLES2-3 + KHR
# entrypoints that Flutter's libflutter.so imports (its DT_NEEDED libEGL.so/libGLESv2.so undefined
# symbols). No GPL-3.0 sources.
#
# Generate the bionic->host-Mesa GL forwarders and their matching symbol table:
#   src/ndk/bionic/acl_egl_fwd.c    bionic libEGL.so    (the EGL trampolines + bridge global)
#   src/ndk/bionic/acl_gles_fwd.c   bionic libGLESv2.so (the GLES trampolines + bridge global)
#   src/ndk/gl_forward_syms.inc     the ABI-ordered name table acl uses to resolve host Mesa
#
# Each forwarder is a tiny naked trampoline: save the bionic %fs, wrfsbase to the GL donor's glibc
# %fs (bridge->donor_fs, where Mesa keeps its per-thread TLS), tail-call host Mesa via
# bridge->fns[i], then restore the bionic %fs. Forwarder i and name table entry i MUST stay aligned;
# generating all three from this one list is how they stay in lockstep.
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

EGL = [
    "eglChooseConfig", "eglCreateContext", "eglCreateImageKHR", "eglCreatePbufferSurface",
    "eglCreateWindowSurface", "eglDestroyContext", "eglDestroyImageKHR", "eglDestroySurface",
    "eglGetCurrentContext", "eglGetCurrentDisplay", "eglGetCurrentSurface", "eglGetDisplay",
    "eglGetError", "eglGetProcAddress", "eglInitialize", "eglMakeCurrent", "eglQueryString",
    "eglQuerySurface", "eglSwapBuffers", "eglTerminate",
    # EGL config/context query + per-thread teardown (a native GL lib like librive-android.so NEEDs
    # these): eglGetConfigAttrib reads config properties, eglQueryContext reads context attributes,
    # eglReleaseThread frees the thread's EGL state.
    "eglGetConfigAttrib", "eglQueryContext", "eglReleaseThread", "eglBindAPI",
    # EGL_KHR_fence_sync / reusable_sync — libgui.so / libnativedisplay.so (pulled in by YouTube's
    # media stack) NEED these; Mesa resolves them (EGL 1.5 core aliases, via eglGetProcAddress).
    "eglCreateSyncKHR", "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglWaitSyncKHR",
    # Remaining core EGL 1.0-1.5 surface Mesa exports (eglGetConfigs, sync, image, swap-interval,
    # platform display/surface, wait/bind-tex) — system GL libs (libgui/libnativedisplay) NEED them.
    "eglBindTexImage", "eglClientWaitSync", "eglCopyBuffers", "eglCreateImage",
    "eglCreatePbufferFromClientBuffer", "eglCreatePixmapSurface", "eglCreatePlatformPixmapSurface", "eglCreatePlatformWindowSurface",
    "eglCreateSync", "eglDestroyImage", "eglDestroySync", "eglGetConfigs",
    "eglGetPlatformDisplay", "eglGetSyncAttrib", "eglQueryAPI", "eglReleaseTexImage",
    "eglSurfaceAttrib", "eglSwapInterval", "eglWaitClient", "eglWaitGL",
    "eglWaitNative", "eglWaitSync",
]
GLES = [
    "glActiveTexture", "glAttachShader", "glBindAttribLocation", "glBindBuffer", "glBindFramebuffer",
    "glBindRenderbuffer", "glBindTexture", "glBlendColor", "glBlendEquation", "glBlendFunc",
    "glBufferData", "glBufferSubData", "glCheckFramebufferStatus", "glClear", "glClearColor",
    "glClearDepthf", "glClearStencil", "glColorMask", "glCompileShader", "glCompressedTexImage2D",
    "glCompressedTexSubImage2D", "glCopyTexSubImage2D", "glCreateProgram", "glCreateShader",
    "glCullFace", "glDeleteBuffers", "glDeleteFramebuffers", "glDeleteProgram",
    "glDeleteRenderbuffers", "glDeleteShader", "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDisable",
    "glDisableVertexAttribArray", "glDrawArrays", "glDrawElements", "glEGLImageTargetTexture2DOES",
    "glEnable", "glEnableVertexAttribArray", "glFinish", "glFlush", "glFramebufferRenderbuffer",
    "glFramebufferTexture2D", "glFrontFace", "glGenBuffers", "glGenerateMipmap", "glGenFramebuffers",
    "glGenRenderbuffers", "glGenTextures", "glGetBufferParameteriv", "glGetError",
    "glGetFramebufferAttachmentParameteriv", "glGetIntegerv", "glGetProgramInfoLog", "glGetProgramiv",
    "glGetRenderbufferParameteriv", "glGetShaderInfoLog", "glGetShaderiv",
    "glGetShaderPrecisionFormat", "glGetString", "glGetAttribLocation", "glGetUniformLocation", "glIsTexture",
    "glLineWidth", "glLinkProgram", "glPixelStorei", "glReadPixels", "glRenderbufferStorage",
    "glScissor", "glShaderSource", "glStencilFunc", "glStencilFuncSeparate", "glStencilMask",
    "glStencilMaskSeparate", "glStencilOp", "glStencilOpSeparate", "glTexImage2D", "glTexParameterf",
    "glTexParameterfv", "glTexParameteri", "glTexParameteriv", "glTexSubImage2D", "glUniform1f",
    "glUniform1fv", "glUniform1i", "glUniform1iv", "glUniform2f", "glUniform2fv", "glUniform2i",
    "glUniform2iv", "glUniform3f", "glUniform3fv", "glUniform3i", "glUniform3iv", "glUniform4f",
    "glUniform4fv", "glUniform4i", "glUniform4iv", "glUniformMatrix2fv", "glUniformMatrix3fv",
    "glUniformMatrix4fv", "glUseProgram", "glVertexAttrib1f", "glVertexAttrib2fv", "glVertexAttrib3fv",
    "glVertexAttrib4fv", "glVertexAttribPointer", "glViewport",
    # GLES3.0/3.1 core: VAOs, instancing, MRT/blit, transform-feedback-less UBOs, immutable textures,
    # buffer mapping and image load/store. A modern native GL runtime (librive-android.so — Rive
    # vector animation) NEEDs these; without them its dlopen fails on an unresolved GLES3 symbol.
    "glBindBufferRange", "glBindImageTexture", "glBindVertexArray", "glBlitFramebuffer",
    "glClearBufferfv", "glClearBufferiv", "glClearBufferuiv", "glDeleteVertexArrays",
    "glDepthRangef", "glDrawArraysInstanced", "glDrawBuffers", "glDrawElementsInstanced",
    "glFramebufferParameteri", "glGenVertexArrays", "glGetStringi", "glGetUniformBlockIndex",
    "glInvalidateFramebuffer", "glMapBufferRange", "glMemoryBarrier",
    "glRenderbufferStorageMultisample", "glTexStorage2D", "glUniformBlockBinding", "glUnmapBuffer",
    "glVertexAttribDivisor", "glVertexAttribIPointer",
    # GLES3.x completeness (YouTube libgoogle3.so): transform feedback, compute, VAOs,
    # samplers, sync objects, program-binary, immutable 3D textures, uint uniforms, etc.
    "glBeginQuery", "glBeginTransformFeedback", "glBindBufferBase", "glBindSampler",
    "glBindTransformFeedback", "glBlendEquationSeparate", "glBlendFuncSeparate", "glClearBufferfi",
    "glClientWaitSync", "glCompressedTexImage3D", "glCompressedTexSubImage3D", "glCopyBufferSubData",
    "glCopyTexImage2D", "glCopyTexSubImage3D", "glDeleteQueries", "glDeleteSamplers",
    "glDeleteSync", "glDeleteTransformFeedbacks", "glDetachShader", "glDispatchCompute",
    "glDrawRangeElements", "glEndQuery", "glEndTransformFeedback", "glFenceSync",
    "glFramebufferTextureLayer", "glGenQueries", "glGenSamplers", "glGenTransformFeedbacks",
    "glGetActiveAttrib", "glGetActiveUniform", "glGetActiveUniformBlockiv", "glGetActiveUniformBlockName",
    "glGetActiveUniformsiv", "glGetAttachedShaders", "glGetBooleanv", "glGetBufferParameteri64v",
    "glGetFloatv", "glGetFragDataLocation", "glGetInteger64i_v", "glGetInteger64v",
    "glGetIntegeri_v", "glGetInternalformativ", "glGetProgramBinary", "glGetQueryiv",
    "glGetQueryObjectuiv", "glGetSamplerParameterfv", "glGetSamplerParameteriv", "glGetShaderSource",
    "glGetSynciv", "glGetTexParameterfv", "glGetTexParameteriv", "glGetTransformFeedbackVarying",
    "glGetUniformIndices", "glGetVertexAttribfv", "glGetVertexAttribiv", "glGetVertexAttribPointerv",
    "glHint", "glInvalidateSubFramebuffer", "glIsBuffer", "glIsEnabled",
    "glIsFramebuffer", "glIsProgram", "glIsQuery", "glIsRenderbuffer",
    "glIsSampler", "glIsShader", "glIsSync", "glIsTransformFeedback",
    "glIsVertexArray", "glPauseTransformFeedback", "glPolygonOffset", "glProgramBinary",
    "glProgramUniform1f", "glProgramUniform1i", "glProgramUniform1ui", "glProgramUniform2f",
    "glProgramUniform2i", "glProgramUniform2iv", "glProgramUniform4f", "glProgramUniform4fv",
    "glProgramUniform4i", "glProgramUniform4ui", "glProgramUniformMatrix3fv", "glProgramUniformMatrix4fv",
    "glReadBuffer", "glResumeTransformFeedback", "glSampleCoverage", "glSamplerParameterf",
    "glSamplerParameteri", "glTexBuffer", "glTexImage3D", "glTexStorage3D",
    "glTexSubImage3D", "glTransformFeedbackVaryings", "glUniform1ui", "glUniform1uiv",
    "glUniform2ui", "glUniform2uiv", "glUniform3ui", "glUniform3uiv",
    "glUniform4ui", "glUniform4uiv", "glUniformMatrix2x3fv", "glUniformMatrix2x4fv",
    "glUniformMatrix3x2fv", "glUniformMatrix3x4fv", "glUniformMatrix4x2fv", "glUniformMatrix4x3fv",
    "glValidateProgram", "glVertexAttrib1fv", "glVertexAttrib2f", "glVertexAttrib3f",
    "glVertexAttrib4f", "glVertexAttribI4i", "glVertexAttribI4iv", "glVertexAttribI4ui",
    "glVertexAttribI4uiv", "glWaitSync",
    # The rest of the GLES 2.0-3.2 entry points that Mesa's libGLESv2 exports: a native library
    # that imports one a4l does not forward fails to load at all (libwhatsapp.so imports
    # glGetUniformiv). glDebugMessageCallback is left out: its callback would run on Mesa's side
    # of the %fs swap.
    "glActiveShaderProgram", "glBindProgramPipeline", "glBindVertexBuffer", "glBlendBarrier",
    "glBlendEquationSeparatei", "glBlendEquationi", "glBlendFuncSeparatei", "glBlendFunci",
    "glColorMaski", "glCopyImageSubData", "glCreateShaderProgramv", "glDebugMessageControl",
    "glDebugMessageInsert", "glDeleteProgramPipelines", "glDisablei",
    "glDispatchComputeIndirect", "glDrawArraysIndirect", "glDrawElementsBaseVertex",
    "glDrawElementsIndirect", "glDrawElementsInstancedBaseVertex",
    "glDrawRangeElementsBaseVertex", "glEnablei", "glFlushMappedBufferRange",
    "glFramebufferTexture", "glGenProgramPipelines", "glGetBooleani_v", "glGetBufferPointerv",
    "glGetDebugMessageLog", "glGetFramebufferParameteriv", "glGetGraphicsResetStatus",
    "glGetMultisamplefv", "glGetObjectLabel", "glGetObjectPtrLabel", "glGetPointerv",
    "glGetProgramInterfaceiv", "glGetProgramPipelineInfoLog", "glGetProgramPipelineiv",
    "glGetProgramResourceIndex", "glGetProgramResourceLocation", "glGetProgramResourceName",
    "glGetProgramResourceiv", "glGetSamplerParameterIiv", "glGetSamplerParameterIuiv",
    "glGetTexLevelParameterfv", "glGetTexLevelParameteriv", "glGetTexParameterIiv",
    "glGetTexParameterIuiv", "glGetUniformfv", "glGetUniformiv", "glGetUniformuiv",
    "glGetVertexAttribIiv", "glGetVertexAttribIuiv", "glGetnUniformfv", "glGetnUniformiv",
    "glGetnUniformuiv", "glIsEnabledi", "glIsProgramPipeline", "glMemoryBarrierByRegion",
    "glMinSampleShading", "glObjectLabel", "glObjectPtrLabel", "glPatchParameteri",
    "glPopDebugGroup", "glPrimitiveBoundingBox", "glProgramParameteri", "glProgramUniform1fv",
    "glProgramUniform1iv", "glProgramUniform1uiv", "glProgramUniform2fv", "glProgramUniform2ui",
    "glProgramUniform2uiv", "glProgramUniform3f", "glProgramUniform3fv", "glProgramUniform3i",
    "glProgramUniform3iv", "glProgramUniform3ui", "glProgramUniform3uiv", "glProgramUniform4iv",
    "glProgramUniform4uiv", "glProgramUniformMatrix2fv", "glProgramUniformMatrix2x3fv",
    "glProgramUniformMatrix2x4fv", "glProgramUniformMatrix3x2fv", "glProgramUniformMatrix3x4fv",
    "glProgramUniformMatrix4x2fv", "glProgramUniformMatrix4x3fv", "glPushDebugGroup",
    "glReadnPixels", "glReleaseShaderCompiler", "glSampleMaski", "glSamplerParameterIiv",
    "glSamplerParameterIuiv", "glSamplerParameterfv", "glSamplerParameteriv", "glShaderBinary",
    "glTexBufferRange", "glTexParameterIiv", "glTexParameterIuiv", "glTexStorage2DMultisample",
    "glTexStorage3DMultisample", "glUseProgramStages", "glValidateProgramPipeline",
    "glVertexAttribBinding", "glVertexAttribFormat", "glVertexAttribIFormat",
    "glVertexBindingDivisor",
]

BANNER = "// GENERATED by tools/gen-gl-forwarders.py — do not hand-edit."


# The per-thread-donor helper, defined ONCE in libEGL.so and called by BOTH libs' forwarders (and the
# JIT eglGetProcAddress stubs). It maps the caller's bionic thread to its OWN glibc GL donor %fs so
# host Mesa's per-thread state (current context, driver TLS) is never shared between two GL threads —
# the raster + IO threads Flutter drives GL from would otherwise collide on one donor and corrupt
# memory. It caches the donor in a bionic __thread var (native bionic TLS, no boundary), and on first
# use grabs a slot from a pool acl fills at install. CRUCIAL: it clobbers ONLY rax/r10/r11 + flags, so
# a forwarder can call it with the real args still live in rdi..r9/xmm.
DONOR_SUPPORT = r'''
// Per-thread donor MAP: {bionic_fs, donor_fs} pairs, keyed by the caller's bionic %fs. No __thread —
// bionic rejects initial-exec TLS in a dlopen'd lib, and a global lock-free table needs none. 8 pairs
// (= kGlDonorPool); zero-initialized. acl fills acl_gl_donor_pool at install (slot i -> pool[i]);
// pick_donor claims a slot the first time each thread appears and caches its donor there.
uintptr_t acl_gl_donor_map[16] = {0};   // 8 pairs of {bionic_fs, donor_fs}; MUST match the 128 below
uintptr_t acl_gl_donor_pool[16] = {0};  // filled by acl (src/ndk/bionic_runtime.cpp)

#if defined(__x86_64__)
// uintptr_t acl_gl_pick_donor(void): rax = the calling bionic thread's own glibc donor %fs. Clobbers
// only rax/r10/r11 (+ rbx, saved/restored) so a forwarder can call it with the GL args still live.
asm(
    ".text\n"
    ".globl acl_gl_pick_donor\n"
    "acl_gl_pick_donor:\n"
    "  push %rbx\n"
    "  rdfsbase %rbx\n"                                   // rbx = caller bionic %fs (the key)
    "  mov acl_gl_donor_map@GOTPCREL(%rip), %r10\n"       // r10 = &map[0]
    "  lea 128(%r10), %r11\n"                             // r11 = end (8 pairs * 16 bytes)
    ".Lscan:\n"
    "  cmp %r11, %r10\n"
    "  jae .Lfallback\n"                                  // table full (>8 GL threads)
    "  mov (%r10), %rax\n"                                // slot.bionic_fs
    "  cmp %rax, %rbx\n"
    "  je .Lfound\n"                                      // our thread -> use cached donor
    "  test %rax, %rax\n"
    "  je .Lclaim\n"                                      // empty slot -> claim it
    "  add $16, %r10\n"
    "  jmp .Lscan\n"
    ".Lfound:\n"
    "  mov 8(%r10), %rax\n"                               // rax = slot.donor_fs
    "  pop %rbx\n"
    "  ret\n"
    ".Lclaim:\n"
    "  xor %eax, %eax\n"                                  // expect empty (0)
    "  lock cmpxchg %rbx, (%r10)\n"                       // claim: (r10)=key iff it was 0
    "  jne .Lscan\n"                                      // lost the race -> rescan this slot
    "  mov acl_gl_donor_map@GOTPCREL(%rip), %r11\n"       // index = (slot - base)/16
    "  mov %r10, %rax\n"
    "  sub %r11, %rax\n"
    "  shr $4, %rax\n"
    "  mov acl_gl_donor_pool@GOTPCREL(%rip), %r11\n"
    "  mov (%r11,%rax,8), %rax\n"                         // rax = pool[index]
    "  mov %rax, 8(%r10)\n"                               // publish slot.donor_fs
    "  pop %rbx\n"
    "  ret\n"
    ".Lfallback:\n"
    "  mov acl_gl_donor_pool@GOTPCREL(%rip), %rax\n"
    "  mov (%rax), %rax\n"                                // pool[0]
    "  pop %rbx\n"
    "  ret\n"
);
#elif defined(__aarch64__)
// The same table on arm64, in three entry points.
//
// acl_gl_pick_donor_x9: x9 = the calling bionic thread's donor thread pointer. Uses x9 to x13 and
// nothing else, so a forwarder can call it with the GL arguments live.
// acl_gl_pick_donor: the same for a C caller, in x0.
// acl_gl_call_x15: call the function in x15 on the donor thread pointer and come back on the
// caller's. Every forwarder ends here. Arguments in registers are untouched. The first eight
// stack slots are copied below the frame, which holds what must outlive the call: the return
// address and the caller's thread pointer.
asm(
    ".text\n"
    ".globl acl_gl_pick_donor_x9\n"
    ".type acl_gl_pick_donor_x9,%function\n"
    "acl_gl_pick_donor_x9:\n"
    "  mrs x10, tpidr_el0\n"                              // the key
    "  adrp x11, :got:acl_gl_donor_map\n"
    "  ldr x11, [x11, :got_lo12:acl_gl_donor_map]\n"      // x11 = &map[0]
    "  mov x13, x11\n"
    "1:\n"
    "  add x12, x11, #128\n"                              // end (8 pairs * 16 bytes)
    "  cmp x13, x12\n"
    "  b.hs 4f\n"                                         // table full (>8 GL threads)
    "  ldr x9, [x13]\n"                                   // slot.bionic_tp
    "  cmp x9, x10\n"
    "  b.eq 2f\n"                                         // our thread -> its donor
    "  cbz x9, 3f\n"                                      // empty slot -> claim it
    "  add x13, x13, #16\n"
    "  b 1b\n"
    "2:\n"
    "  ldr x9, [x13, #8]\n"
    "  ret\n"
    "3:\n"
    "  ldxr x9, [x13]\n"
    "  cbnz x9, 5f\n"                                     // lost the race -> look at it again
    "  stxr w9, x10, [x13]\n"
    "  cbnz w9, 3b\n"
    "  sub x9, x13, x11\n"                                // slot index * 16
    "  lsr x9, x9, #1\n"                                  // slot index * 8
    "  adrp x12, :got:acl_gl_donor_pool\n"
    "  ldr x12, [x12, :got_lo12:acl_gl_donor_pool]\n"
    "  ldr x9, [x12, x9]\n"                               // pool[index]
    "  str x9, [x13, #8]\n"                               // publish slot.donor
    "  ret\n"
    "5:\n"
    "  clrex\n"
    "  b 1b\n"
    "4:\n"
    "  adrp x12, :got:acl_gl_donor_pool\n"
    "  ldr x12, [x12, :got_lo12:acl_gl_donor_pool]\n"
    "  ldr x9, [x12]\n"                                   // pool[0]
    "  ret\n"
    ".size acl_gl_pick_donor_x9,.-acl_gl_pick_donor_x9\n"
    ".globl acl_gl_pick_donor\n"
    ".type acl_gl_pick_donor,%function\n"
    "acl_gl_pick_donor:\n"
    "  mov x14, x30\n"
    "  bl acl_gl_pick_donor_x9\n"
    "  mov x0, x9\n"
    "  ret x14\n"
    ".size acl_gl_pick_donor,.-acl_gl_pick_donor\n"
    ".globl acl_gl_call_x15\n"
    ".type acl_gl_call_x15,%function\n"
    "acl_gl_call_x15:\n"
    "  sub sp, sp, #96\n"                                 // [sp..sp+64) the callee's stack args
    "  stp x29, x30, [sp, #64]\n"
    "  add x29, sp, #64\n"
    "  bl acl_gl_pick_donor_x9\n"                         // x9 = donor
    "  mrs x10, tpidr_el0\n"
    "  str x10, [sp, #80]\n"                              // the caller's thread pointer
    "  ldp x11, x12, [sp, #96]\n"
    "  stp x11, x12, [sp, #0]\n"
    "  ldp x11, x12, [sp, #112]\n"
    "  stp x11, x12, [sp, #16]\n"
    "  ldp x11, x12, [sp, #128]\n"
    "  stp x11, x12, [sp, #32]\n"
    "  ldp x11, x12, [sp, #144]\n"
    "  stp x11, x12, [sp, #48]\n"
    "  msr tpidr_el0, x9\n"
    "  blr x15\n"
    "  ldr x10, [sp, #80]\n"
    "  msr tpidr_el0, x10\n"
    "  ldp x29, x30, [sp, #64]\n"
    "  add sp, sp, #96\n"
    "  ret\n"
    ".size acl_gl_call_x15,.-acl_gl_call_x15\n"
);
#endif

// eglGetNativeClientBufferANDROID: the engine's impeller ProcTable dlsyms this from libEGL.so to turn
// an AHardwareBuffer* into an EGLClientBuffer for eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID). On acl
// the AHardwareBuffer* IS the client buffer our create-image M2 shim expects, so return it directly.
// Pure passthrough (no Mesa, no %fs bridge). Provenance: EGL_ANDROID_get_native_client_buffer. No GPL-3.0 sources.
__attribute__((visibility("default"))) void* eglGetNativeClientBufferANDROID(const void* buffer) {
  return (void*)buffer;
}

// eglPresentationTimeANDROID: EGL_ANDROID_presentation_time — set a surface's presentation time.
// Desktop Mesa/EGL has no such entrypoint, so this is a no-op returning EGL_TRUE; the symbol must
// exist (YouTube's libgoogle3.so references it) or its dlopen fails. Provenance: the EGL_ANDROID_
// presentation_time extension spec. No GPL-3.0 sources.
__attribute__((visibility("default"))) unsigned int eglPresentationTimeANDROID(void* dpy, void* sur, long long time) {
  (void)dpy; (void)sur; (void)time; return 1u;  // EGL_TRUE
}

// eglDupNativeFenceFDANDROID: EGL_ANDROID_native_fence_sync — dup a sync's native fence fd. No
// such fence on desktop Mesa, so return EGL_NO_NATIVE_FENCE_FD_ANDROID (-1); callers CPU-sync.
__attribute__((visibility("default"))) int eglDupNativeFenceFDANDROID(void* dpy, void* sync) {
  (void)dpy; (void)sync; return -1;  // EGL_NO_NATIVE_FENCE_FD_ANDROID
}
'''


def forwarders(syms, bridge, soname, with_support):
    out = [
        # REUSE-IgnoreStart
        "// SPDX-License-Identifier: Apache-2.0",
        # REUSE-IgnoreEnd
        "// Provenance: original acl design. bionic->host-Mesa GL forwarders; see",
        "// tools/gen-gl-forwarders.py and docs/ndk-scope.md. No GPL-3.0 sources.",
        f"// {BANNER}",
        "//",
        f"// Staged as bionic {soname}. Each entrypoint resolves THIS thread's glibc GL donor %fs via",
        "// acl_gl_pick_donor, %fs-swaps to it (that is Mesa's per-thread TLS home), calls host Mesa",
        "// via bridge->fns[i], and swaps back. acl writes the bridge pointer after this lib loads.",
        "#include <stdint.h>",
        "",
        "uintptr_t acl_gl_pick_donor(void);",
        "// { uintptr_t donor_fs; void* fns[]; } — populated by acl (src/ndk/bionic_runtime.cpp).",
        f"void* {bridge} = 0;",
    ]
    if with_support:
        out.append(DONOR_SUPPORT.rstrip("\n"))
    # A forwarder cannot just push the saved %fs and call Mesa: a GL entry with >6 integer args (e.g.
    # glGetActiveUniform's `name` out-buffer, glTexImage2D's `data`) passes the extras ON THE STACK,
    # and any push shifts them — Mesa would read the saved %fs as an arg pointer and scribble a uniform
    # name over the bionic TCB. So build a frame and COPY the stack args below a fresh call, exactly as
    # make_downcall does. Copy 8 slots (covers 6+8=14 args; every GLES entry). The caller's bionic %fs
    # and this thread's donor are stashed at [rbp-8]/[rbp-16], below the frame, out of the args' way.
    copy = []
    for k in range(7, -1, -1):  # push slot7..slot0 so slot0 lands just above Mesa's return address
        copy.append(f'    "  mov {16 + 8 * k}(%rbp), %rax\\n  push %rax\\n"')  # app arg at [rbp+16+8k]
    out += ["", "#if defined(__x86_64__)", 'asm(', '    ".text\\n"']
    for i, s in enumerate(syms):
        off = 8 + 8 * i  # bridge->fns[i]; donor_fs occupies offset 0 (unused now)
        out += [
            f'    ".globl {s}\\n"',
            f'    "{s}:\\n"',
            '    "  push %rbp\\n"',
            '    "  mov %rsp, %rbp\\n"',                   # [rbp+8]=ret, [rbp+16+8k]=app stack args
            '    "  call acl_gl_pick_donor@PLT\\n"',       # rax = this thread's donor (args stay live)
            '    "  rdfsbase %r11\\n"',                    # r11 = caller bionic %fs
            '    "  mov %rax, %r10\\n"',                   # r10 = donor
            '    "  push %r11\\n"',                        # [rbp-8]  = bionic %fs
            '    "  push %r10\\n"',                        # [rbp-16] = donor
        ] + copy + [
            '    "  wrfsbase %r10\\n"',                    # %fs = donor (r10 still holds it)
            f'    "  mov {bridge}@GOTPCREL(%rip), %r10\\n"',
            '    "  mov (%r10), %r10\\n"',                 # bridge*
            f'    "  mov {off}(%r10), %r10\\n"',           # bridge->fns[i] (host Mesa {s})
            '    "  call *%r10\\n"',                       # Mesa (rsp 16-aligned; copied stack args in place)
            '    "  mov -8(%rbp), %r11\\n"',               # caller bionic %fs
            '    "  wrfsbase %r11\\n"',                    # %fs = bionic
            '    "  mov %rbp, %rsp\\n"',
            '    "  pop %rbp\\n"',
            '    "  ret\\n"',
        ]
    out.append(");")
    # arm64: load bridge->fns[i] and go to acl_gl_call_x15 in libEGL.so, which does the rest.
    # x16 and x17 are the linker's between the branch and its target; x15 is not.
    out += ["#elif defined(__aarch64__)", 'asm(', '    ".text\\n"']
    for i, s in enumerate(syms):
        off = 8 + 8 * i
        out += [
            f'    ".globl {s}\\n"',
            f'    ".type {s},%function\\n"',
            f'    "{s}:\\n"',
            f'    "  adrp x15, :got:{bridge}\\n"',
            f'    "  ldr x15, [x15, :got_lo12:{bridge}]\\n"',
            '    "  ldr x15, [x15]\\n"',                 # bridge*
            f'    "  ldr x15, [x15, #{off}]\\n"',        # bridge->fns[i]
            '    "  b acl_gl_call_x15\\n"',
        ]
    out.append(");")
    out.append("#endif")
    return "\n".join(out) + "\n"


def sym_table(egl, gles):
    def arr(name, syms):
        lines = [f"static const char* const {name}[] = {{"]
        lines += [f'    "{s}",' for s in syms]
        lines.append("};")
        return "\n".join(lines)
    return (
        # REUSE-IgnoreStart
        "// SPDX-License-Identifier: Apache-2.0\n"
        # REUSE-IgnoreEnd
        "// Provenance: original acl design; standard Khronos EGL/GLES entrypoint names. No GPL-3.0 sources.\n"
        f"// {BANNER} The ORDER is the ABI contract with acl_egl_fwd.c / acl_gles_fwd.c:\n"
        "// host-Mesa symbol N is what forwarder N calls. Regenerate both from the same source.\n"
        + arr("kEglForwardSyms", egl) + "\n\n" + arr("kGlesForwardSyms", gles) + "\n"
    )


def main():
    # libEGL.so carries the shared per-thread-donor helper; libGLESv2.so imports it (DT_NEEDED libEGL).
    with open(os.path.join(ROOT, "src/ndk/bionic/acl_egl_fwd.c"), "w") as f:
        f.write(forwarders(EGL, "acl_gl_egl_bridge", "libEGL.so", with_support=True))
    with open(os.path.join(ROOT, "src/ndk/bionic/acl_gles_fwd.c"), "w") as f:
        f.write(forwarders(GLES, "acl_gl_gles_bridge", "libGLESv2.so", with_support=False))
    with open(os.path.join(ROOT, "src/ndk/gl_forward_syms.inc"), "w") as f:
        f.write(sym_table(EGL, GLES))
    print(f"generated: EGL={len(EGL)} GLES={len(GLES)} forwarders + symbol table")


if __name__ == "__main__":
    main()

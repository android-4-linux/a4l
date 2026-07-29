// SPDX-License-Identifier: Apache-2.0
// Provenance: Khronos EGL 1.4 spec + EGL/egl.h; JNI contract of AOSP android.opengl.EGL14
// (frameworks/base/core/jni/android_opengl_EGL14.cpp — behavioral: method set, signatures, and the
// wrapper-object handle model). Re-authored as thin passthroughs to host Mesa libEGL. No GPL-3.0 sources.

#include "gfx/egl14_jni.h"

#if ACL_HAVE_GL

// JNINativeMethod.{name,signature} are char* (not const); the table strings are read-only. Same
// pragma the AOSP glgen JNI files use.
#pragma GCC diagnostic ignored "-Wwrite-strings"

#include <EGL/egl.h>
#include <cstdint>
#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "acl.gfx.egl14";

// android.opengl.EGL{Display,Context,Surface,Config} wrapper classes + their `long`-handle ctor /
// getNativeHandle() accessor, and the EGL_NO_* singletons — cached in _nativeClassInit.
jclass g_dpy_cls = nullptr, g_ctx_cls = nullptr, g_sur_cls = nullptr, g_cfg_cls = nullptr;
jmethodID g_dpy_get = nullptr, g_ctx_get = nullptr, g_sur_get = nullptr, g_cfg_get = nullptr;
jmethodID g_dpy_ctor = nullptr, g_ctx_ctor = nullptr, g_sur_ctor = nullptr, g_cfg_ctor = nullptr;
jobject g_no_ctx = nullptr, g_no_dpy = nullptr, g_no_sur = nullptr;

// Java handle object -> native EGL pointer. Null object -> nullptr (the EGL_NO_* wrappers hold 0).
void* from_handle(JNIEnv* e, jmethodID get, jobject o) {
  if (o == nullptr) return nullptr;
  return reinterpret_cast<void*>(e->CallLongMethod(o, get));
}
// Native EGL pointer -> Java handle object, reusing the EGL_NO_* singletons.
jobject to_handle(JNIEnv* e, jclass cls, jmethodID ctor, void* h) {
  if (cls == g_ctx_cls && reinterpret_cast<EGLContext>(h) == EGL_NO_CONTEXT) return g_no_ctx;
  if (cls == g_dpy_cls && reinterpret_cast<EGLDisplay>(h) == EGL_NO_DISPLAY) return g_no_dpy;
  if (cls == g_sur_cls && reinterpret_cast<EGLSurface>(h) == EGL_NO_SURFACE) return g_no_sur;
  return e->NewObject(cls, ctor, reinterpret_cast<jlong>(h));
}

void JNICALL nativeClassInit(JNIEnv* e, jclass) {
  auto pin = [&](const char* name) -> jclass {
    jclass local = e->FindClass(name);
    jclass g = local != nullptr ? static_cast<jclass>(e->NewGlobalRef(local)) : nullptr;
    if (local != nullptr) e->DeleteLocalRef(local);
    return g;
  };
  g_dpy_cls = pin("android/opengl/EGLDisplay");
  g_ctx_cls = pin("android/opengl/EGLContext");
  g_sur_cls = pin("android/opengl/EGLSurface");
  g_cfg_cls = pin("android/opengl/EGLConfig");
  if (!g_dpy_cls || !g_ctx_cls || !g_sur_cls || !g_cfg_cls) {
    log::w(kTag, "EGL14: handle classes not found");
    return;
  }
  g_dpy_get = e->GetMethodID(g_dpy_cls, "getNativeHandle", "()J");
  g_ctx_get = e->GetMethodID(g_ctx_cls, "getNativeHandle", "()J");
  g_sur_get = e->GetMethodID(g_sur_cls, "getNativeHandle", "()J");
  g_cfg_get = e->GetMethodID(g_cfg_cls, "getNativeHandle", "()J");
  g_dpy_ctor = e->GetMethodID(g_dpy_cls, "<init>", "(J)V");
  g_ctx_ctor = e->GetMethodID(g_ctx_cls, "<init>", "(J)V");
  g_sur_ctor = e->GetMethodID(g_sur_cls, "<init>", "(J)V");
  g_cfg_ctor = e->GetMethodID(g_cfg_cls, "<init>", "(J)V");

  g_no_ctx = e->NewGlobalRef(e->NewObject(g_ctx_cls, g_ctx_ctor, reinterpret_cast<jlong>(EGL_NO_CONTEXT)));
  g_no_dpy = e->NewGlobalRef(e->NewObject(g_dpy_cls, g_dpy_ctor, reinterpret_cast<jlong>(EGL_NO_DISPLAY)));
  g_no_sur = e->NewGlobalRef(e->NewObject(g_sur_cls, g_sur_ctor, reinterpret_cast<jlong>(EGL_NO_SURFACE)));
  // Point EGL14.EGL_NO_{CONTEXT,DISPLAY,SURFACE} at those singletons (the Java class declares them
  // null; EGL14.<clinit> relies on nativeClassInit to fill them).
  jclass egl14 = e->FindClass("android/opengl/EGL14");
  if (egl14 != nullptr) {
    e->SetStaticObjectField(egl14, e->GetStaticFieldID(egl14, "EGL_NO_CONTEXT", "Landroid/opengl/EGLContext;"), g_no_ctx);
    e->SetStaticObjectField(egl14, e->GetStaticFieldID(egl14, "EGL_NO_DISPLAY", "Landroid/opengl/EGLDisplay;"), g_no_dpy);
    e->SetStaticObjectField(egl14, e->GetStaticFieldID(egl14, "EGL_NO_SURFACE", "Landroid/opengl/EGLSurface;"), g_no_sur);
    e->DeleteLocalRef(egl14);
  }
}

// --- passthroughs to host Mesa libEGL ----------------------------------------------------------
jint JNICALL EGL14_eglGetError(JNIEnv*, jclass) { return eglGetError(); }

jobject JNICALL EGL14_eglGetDisplayInt(JNIEnv* e, jclass, jint id) {
  return to_handle(e, g_dpy_cls, g_dpy_ctor,
                   eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(static_cast<intptr_t>(id))));
}
jobject JNICALL EGL14_eglGetDisplay(JNIEnv* e, jclass, jlong id) {
  return to_handle(e, g_dpy_cls, g_dpy_ctor,
                   eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(static_cast<intptr_t>(id))));
}

jboolean JNICALL EGL14_eglInitialize(JNIEnv* e, jclass, jobject dpy, jintArray major, jint mo,
                                     jintArray minor, jint no) {
  EGLint mj = 0, mn = 0;
  EGLBoolean r = eglInitialize(from_handle(e, g_dpy_get, dpy), &mj, &mn);
  if (major != nullptr) e->SetIntArrayRegion(major, mo, 1, &mj);
  if (minor != nullptr) e->SetIntArrayRegion(minor, no, 1, &mn);
  return r == EGL_TRUE;
}
jboolean JNICALL EGL14_eglTerminate(JNIEnv* e, jclass, jobject dpy) {
  return eglTerminate(from_handle(e, g_dpy_get, dpy)) == EGL_TRUE;
}
jstring JNICALL EGL14_eglQueryString(JNIEnv* e, jclass, jobject dpy, jint name) {
  const char* s = eglQueryString(from_handle(e, g_dpy_get, dpy), name);
  return e->NewStringUTF(s != nullptr ? s : "");
}

jboolean JNICALL EGL14_eglChooseConfig(JNIEnv* e, jclass, jobject dpy, jintArray attribs, jint ao,
                                       jobjectArray configs, jint co, jint configSize,
                                       jintArray num, jint no) {
  EGLint* a = attribs != nullptr ? e->GetIntArrayElements(attribs, nullptr) : nullptr;
  EGLConfig* c = configs != nullptr ? new EGLConfig[configSize] : nullptr;
  EGLint n = 0;
  EGLBoolean r = eglChooseConfig(from_handle(e, g_dpy_get, dpy), a != nullptr ? a + ao : nullptr,
                                 c, configSize, &n);
  if (r == EGL_TRUE && configs != nullptr) {
    for (EGLint i = 0; i < n && (co + i) < e->GetArrayLength(configs); ++i)
      e->SetObjectArrayElement(configs, co + i, to_handle(e, g_cfg_cls, g_cfg_ctor, c[i]));
  }
  if (num != nullptr) e->SetIntArrayRegion(num, no, 1, &n);
  if (a != nullptr) e->ReleaseIntArrayElements(attribs, a, JNI_ABORT);
  delete[] c;
  return r == EGL_TRUE;
}
jboolean JNICALL EGL14_eglGetConfigAttrib(JNIEnv* e, jclass, jobject dpy, jobject cfg, jint attr,
                                          jintArray val, jint vo) {
  EGLint v = 0;
  EGLBoolean r = eglGetConfigAttrib(from_handle(e, g_dpy_get, dpy),
                                    static_cast<EGLConfig>(from_handle(e, g_cfg_get, cfg)), attr, &v);
  if (val != nullptr) e->SetIntArrayRegion(val, vo, 1, &v);
  return r == EGL_TRUE;
}

jobject JNICALL EGL14_eglCreateContext(JNIEnv* e, jclass, jobject dpy, jobject cfg, jobject share,
                                       jintArray attribs, jint ao) {
  EGLint* a = attribs != nullptr ? e->GetIntArrayElements(attribs, nullptr) : nullptr;
  EGLContext ctx = eglCreateContext(from_handle(e, g_dpy_get, dpy),
                                    static_cast<EGLConfig>(from_handle(e, g_cfg_get, cfg)),
                                    static_cast<EGLContext>(from_handle(e, g_ctx_get, share)),
                                    a != nullptr ? a + ao : nullptr);
  if (a != nullptr) e->ReleaseIntArrayElements(attribs, a, JNI_ABORT);
  return to_handle(e, g_ctx_cls, g_ctx_ctor, ctx);
}
jboolean JNICALL EGL14_eglDestroyContext(JNIEnv* e, jclass, jobject dpy, jobject ctx) {
  return eglDestroyContext(from_handle(e, g_dpy_get, dpy),
                           static_cast<EGLContext>(from_handle(e, g_ctx_get, ctx))) == EGL_TRUE;
}

jobject JNICALL EGL14_eglCreatePbufferSurface(JNIEnv* e, jclass, jobject dpy, jobject cfg,
                                              jintArray attribs, jint ao) {
  EGLint* a = attribs != nullptr ? e->GetIntArrayElements(attribs, nullptr) : nullptr;
  EGLSurface s = eglCreatePbufferSurface(from_handle(e, g_dpy_get, dpy),
                                         static_cast<EGLConfig>(from_handle(e, g_cfg_get, cfg)),
                                         a != nullptr ? a + ao : nullptr);
  if (a != nullptr) e->ReleaseIntArrayElements(attribs, a, JNI_ABORT);
  return to_handle(e, g_sur_cls, g_sur_ctor, s);
}
jboolean JNICALL EGL14_eglDestroySurface(JNIEnv* e, jclass, jobject dpy, jobject sur) {
  return eglDestroySurface(from_handle(e, g_dpy_get, dpy),
                           static_cast<EGLSurface>(from_handle(e, g_sur_get, sur))) == EGL_TRUE;
}
jboolean JNICALL EGL14_eglQuerySurface(JNIEnv* e, jclass, jobject dpy, jobject sur, jint attr,
                                       jintArray val, jint vo) {
  EGLint v = 0;
  EGLBoolean r = eglQuerySurface(from_handle(e, g_dpy_get, dpy),
                                 static_cast<EGLSurface>(from_handle(e, g_sur_get, sur)), attr, &v);
  if (val != nullptr) e->SetIntArrayRegion(val, vo, 1, &v);
  return r == EGL_TRUE;
}

jboolean JNICALL EGL14_eglMakeCurrent(JNIEnv* e, jclass, jobject dpy, jobject draw, jobject read,
                                      jobject ctx) {
  return eglMakeCurrent(from_handle(e, g_dpy_get, dpy),
                        static_cast<EGLSurface>(from_handle(e, g_sur_get, draw)),
                        static_cast<EGLSurface>(from_handle(e, g_sur_get, read)),
                        static_cast<EGLContext>(from_handle(e, g_ctx_get, ctx))) == EGL_TRUE;
}
jobject JNICALL EGL14_eglGetCurrentContext(JNIEnv* e, jclass) {
  return to_handle(e, g_ctx_cls, g_ctx_ctor, eglGetCurrentContext());
}
jobject JNICALL EGL14_eglGetCurrentSurface(JNIEnv* e, jclass, jint which) {
  return to_handle(e, g_sur_cls, g_sur_ctor, eglGetCurrentSurface(which));
}
jobject JNICALL EGL14_eglGetCurrentDisplay(JNIEnv* e, jclass) {
  return to_handle(e, g_dpy_cls, g_dpy_ctor, eglGetCurrentDisplay());
}
jboolean JNICALL EGL14_eglQueryContext(JNIEnv* e, jclass, jobject dpy, jobject ctx, jint attr,
                                       jintArray val, jint vo) {
  EGLint v = 0;
  EGLBoolean r = eglQueryContext(from_handle(e, g_dpy_get, dpy),
                                 static_cast<EGLContext>(from_handle(e, g_ctx_get, ctx)), attr, &v);
  if (val != nullptr) e->SetIntArrayRegion(val, vo, 1, &v);
  return r == EGL_TRUE;
}
jboolean JNICALL EGL14_eglSwapBuffers(JNIEnv* e, jclass, jobject dpy, jobject sur) {
  return eglSwapBuffers(from_handle(e, g_dpy_get, dpy),
                        static_cast<EGLSurface>(from_handle(e, g_sur_get, sur))) == EGL_TRUE;
}
jboolean JNICALL EGL14_eglSwapInterval(JNIEnv* e, jclass, jobject dpy, jint n) {
  return eglSwapInterval(from_handle(e, g_dpy_get, dpy), n) == EGL_TRUE;
}
jboolean JNICALL EGL14_eglSurfaceAttrib(JNIEnv* e, jclass, jobject dpy, jobject sur, jint a, jint v) {
  return eglSurfaceAttrib(from_handle(e, g_dpy_get, dpy),
                          static_cast<EGLSurface>(from_handle(e, g_sur_get, sur)), a, v) == EGL_TRUE;
}
jboolean JNICALL EGL14_eglBindAPI(JNIEnv*, jclass, jint api) { return eglBindAPI(api) == EGL_TRUE; }
jint JNICALL EGL14_eglQueryAPI(JNIEnv*, jclass) { return static_cast<jint>(eglQueryAPI()); }
jboolean JNICALL EGL14_eglWaitClient(JNIEnv*, jclass) { return eglWaitClient() == EGL_TRUE; }
jboolean JNICALL EGL14_eglWaitGL(JNIEnv*, jclass) { return eglWaitGL() == EGL_TRUE; }
jboolean JNICALL EGL14_eglWaitNative(JNIEnv*, jclass, jint eng) { return eglWaitNative(eng) == EGL_TRUE; }
jboolean JNICALL EGL14_eglReleaseThread(JNIEnv*, jclass) { return eglReleaseThread() == EGL_TRUE; }

// A window surface from a Java Surface/SurfaceTexture needs an ANativeWindow bridge into acl's
// surface system; not wired here. Return EGL_NO_SURFACE (apps that only need an off-screen pbuffer —
// ExoPlayer's PlaceholderSurface — never call this). TODO: back with image_reader's ANativeWindow.
jobject JNICALL EGL14_eglCreateWindowSurface(JNIEnv*, jclass, jobject, jobject, jobject, jintArray, jint) {
  log::w(kTag, "eglCreateWindowSurface: window surfaces via EGL14 not supported yet -> EGL_NO_SURFACE");
  return g_no_sur;
}

const JNINativeMethod kMethods[] = {
    {"_nativeClassInit", "()V", reinterpret_cast<void*>(nativeClassInit)},
    {"eglGetError", "()I", reinterpret_cast<void*>(EGL14_eglGetError)},
    {"eglGetDisplay", "(I)Landroid/opengl/EGLDisplay;", reinterpret_cast<void*>(EGL14_eglGetDisplayInt)},
    {"eglGetDisplay", "(J)Landroid/opengl/EGLDisplay;", reinterpret_cast<void*>(EGL14_eglGetDisplay)},
    {"eglInitialize", "(Landroid/opengl/EGLDisplay;[II[II)Z", reinterpret_cast<void*>(EGL14_eglInitialize)},
    {"eglTerminate", "(Landroid/opengl/EGLDisplay;)Z", reinterpret_cast<void*>(EGL14_eglTerminate)},
    {"eglQueryString", "(Landroid/opengl/EGLDisplay;I)Ljava/lang/String;", reinterpret_cast<void*>(EGL14_eglQueryString)},
    {"eglChooseConfig", "(Landroid/opengl/EGLDisplay;[II[Landroid/opengl/EGLConfig;II[II)Z", reinterpret_cast<void*>(EGL14_eglChooseConfig)},
    {"eglGetConfigAttrib", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLConfig;I[II)Z", reinterpret_cast<void*>(EGL14_eglGetConfigAttrib)},
    {"eglCreatePbufferSurface", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLConfig;[II)Landroid/opengl/EGLSurface;", reinterpret_cast<void*>(EGL14_eglCreatePbufferSurface)},
    {"eglDestroySurface", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLSurface;)Z", reinterpret_cast<void*>(EGL14_eglDestroySurface)},
    {"eglQuerySurface", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLSurface;I[II)Z", reinterpret_cast<void*>(EGL14_eglQuerySurface)},
    {"eglBindAPI", "(I)Z", reinterpret_cast<void*>(EGL14_eglBindAPI)},
    {"eglQueryAPI", "()I", reinterpret_cast<void*>(EGL14_eglQueryAPI)},
    {"eglWaitClient", "()Z", reinterpret_cast<void*>(EGL14_eglWaitClient)},
    {"eglReleaseThread", "()Z", reinterpret_cast<void*>(EGL14_eglReleaseThread)},
    {"eglSurfaceAttrib", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLSurface;II)Z", reinterpret_cast<void*>(EGL14_eglSurfaceAttrib)},
    {"eglSwapInterval", "(Landroid/opengl/EGLDisplay;I)Z", reinterpret_cast<void*>(EGL14_eglSwapInterval)},
    {"eglCreateContext", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLConfig;Landroid/opengl/EGLContext;[II)Landroid/opengl/EGLContext;", reinterpret_cast<void*>(EGL14_eglCreateContext)},
    {"eglDestroyContext", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLContext;)Z", reinterpret_cast<void*>(EGL14_eglDestroyContext)},
    {"eglMakeCurrent", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLSurface;Landroid/opengl/EGLSurface;Landroid/opengl/EGLContext;)Z", reinterpret_cast<void*>(EGL14_eglMakeCurrent)},
    {"eglGetCurrentContext", "()Landroid/opengl/EGLContext;", reinterpret_cast<void*>(EGL14_eglGetCurrentContext)},
    {"eglGetCurrentSurface", "(I)Landroid/opengl/EGLSurface;", reinterpret_cast<void*>(EGL14_eglGetCurrentSurface)},
    {"eglGetCurrentDisplay", "()Landroid/opengl/EGLDisplay;", reinterpret_cast<void*>(EGL14_eglGetCurrentDisplay)},
    {"eglQueryContext", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLContext;I[II)Z", reinterpret_cast<void*>(EGL14_eglQueryContext)},
    {"eglWaitGL", "()Z", reinterpret_cast<void*>(EGL14_eglWaitGL)},
    {"eglWaitNative", "(I)Z", reinterpret_cast<void*>(EGL14_eglWaitNative)},
    {"eglSwapBuffers", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLSurface;)Z", reinterpret_cast<void*>(EGL14_eglSwapBuffers)},
    {"_eglCreateWindowSurface", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLConfig;Ljava/lang/Object;[II)Landroid/opengl/EGLSurface;", reinterpret_cast<void*>(EGL14_eglCreateWindowSurface)},
    {"_eglCreateWindowSurfaceTexture", "(Landroid/opengl/EGLDisplay;Landroid/opengl/EGLConfig;Ljava/lang/Object;[II)Landroid/opengl/EGLSurface;", reinterpret_cast<void*>(EGL14_eglCreateWindowSurface)},
};

}  // namespace

jint register_egl14(JNIEnv* env) {
  jclass c = env->FindClass("android/opengl/EGL14");
  if (c == nullptr) { env->ExceptionClear(); log::w(kTag, "android.opengl.EGL14 not found"); return JNI_ERR; }
  const jint rc = env->RegisterNatives(c, kMethods, static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
  env->DeleteLocalRef(c);
  if (rc != JNI_OK) { log::w(kTag, "EGL14 RegisterNatives failed (rc={})", rc); return rc; }
  log::i(kTag, "android.opengl.EGL14 natives registered on host Mesa libEGL");
  return JNI_OK;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

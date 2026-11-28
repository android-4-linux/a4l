// SPDX-License-Identifier: Apache-2.0
// Provenance: Khronos EGL 1.0 spec + EGL/egl.h; the JNI contract of AOSP's
// com.google.android.gles_jni.EGLImpl (frameworks/base/opengl/java + core/jni,
// behavioral: method set, signatures, and the EGL*Impl wrapper-object handle
// model — each EGLDisplay/ EGLContext/EGLConfig/EGLSurface Java object holds a
// `long` native handle). Re-authored (not copied) as thin passthroughs to host
// Mesa libEGL, with two seams delegated to the acl runtime (_eglGetDisplay,
// _eglCreateWindowSurface). No GPL-3.0 sources.

#include "gfx/egl10_jni.h"

#if ACL_HAVE_GL

// JNINativeMethod.{name,signature} are char* (not const); string literals in
// the method table are read-only and never written by JNI. Same pragma the AOSP
// glgen JNI files use.
#pragma GCC diagnostic ignored "-Wwrite-strings"

#include <algorithm>
#include <mutex>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>
#include "gfx/egl_attribs.h"
#include "gfx/gl_egl.h"
#include "log/log.h"

namespace acl::gfx {
namespace {
constexpr const char* kTag = "acl.gfx.egl10";

AclEgl10Hooks g_hooks;

// Cached EGL*Impl wrapper accessors (set in nativeClassInit).
jclass g_config_class = nullptr;     // global ref to EGLConfigImpl
jmethodID g_config_ctor = nullptr;   // EGLConfigImpl(long)
jfieldID g_config_field = nullptr;   // EGLConfigImpl.mEGLConfig  : J
jfieldID g_display_field = nullptr;  // EGLDisplayImpl.mEGLDisplay : J
jfieldID g_context_field = nullptr;  // EGLContextImpl.mEGLContext : J
jfieldID g_surface_field = nullptr;  // EGLSurfaceImpl.mEGLSurface : J

EGLDisplay get_display(JNIEnv* e, jobject o) {
  return o != nullptr
             ? reinterpret_cast<EGLDisplay>(e->GetLongField(o, g_display_field))
             : EGL_NO_DISPLAY;
}
EGLContext get_context(JNIEnv* e, jobject o) {
  return o != nullptr
             ? reinterpret_cast<EGLContext>(e->GetLongField(o, g_context_field))
             : EGL_NO_CONTEXT;
}
EGLSurface get_surface(JNIEnv* e, jobject o) {
  return o != nullptr
             ? reinterpret_cast<EGLSurface>(e->GetLongField(o, g_surface_field))
             : EGL_NO_SURFACE;
}
EGLConfig get_config(JNIEnv* e, jobject o) {
  return o != nullptr
             ? reinterpret_cast<EGLConfig>(e->GetLongField(o, g_config_field))
             : nullptr;
}
jboolean to_jbool(EGLBoolean b) {
  return b == EGL_TRUE ? JNI_TRUE : JNI_FALSE;
}

void throw_iae(JNIEnv* e, const char* msg) {
  if (jclass c = e->FindClass("java/lang/IllegalArgumentException");
      c != nullptr)
    e->ThrowNew(c, msg);
}

// Copy a Java EGL_NONE-terminated int[] into a native vector; empty {EGL_NONE}
// if null.
std::vector<EGLint> attribs_of(JNIEnv* e, jintArray a) {
  if (a == nullptr)
    return {EGL_NONE};
  const jsize n = e->GetArrayLength(a);
  std::vector<EGLint> v(static_cast<std::size_t>(n));
  if (n > 0)
    e->GetIntArrayRegion(a, 0, n, v.data());
  if (v.empty() || v.back() != EGL_NONE)
    v.push_back(EGL_NONE);
  return v;
}

// ── nativeClassInit: cache the wrapper handle fields + EGLConfigImpl ctor
// ──────
void nativeClassInit(JNIEnv* e, jclass) {
  jclass cfg = e->FindClass("com/google/android/gles_jni/EGLConfigImpl");
  g_config_class = static_cast<jclass>(e->NewGlobalRef(cfg));
  g_config_ctor = e->GetMethodID(g_config_class, "<init>", "(J)V");
  g_config_field = e->GetFieldID(g_config_class, "mEGLConfig", "J");
  jclass dpy = e->FindClass("com/google/android/gles_jni/EGLDisplayImpl");
  g_display_field = e->GetFieldID(dpy, "mEGLDisplay", "J");
  jclass ctx = e->FindClass("com/google/android/gles_jni/EGLContextImpl");
  g_context_field = e->GetFieldID(ctx, "mEGLContext", "J");
  jclass sur = e->FindClass("com/google/android/gles_jni/EGLSurfaceImpl");
  g_surface_field = e->GetFieldID(sur, "mEGLSurface", "J");
}

// ── the two acl-delegated seams
// ───────────────────────────────────────────────
jlong _eglGetDisplay(JNIEnv*, jobject, jobject /*native_display*/) {
  // The app asks for EGL_DEFAULT_DISPLAY; hand back acl's wl_display-bound
  // EGLDisplay so its context + the wl_egl_window share acl's one Wayland
  // connection.
  EGLDisplay d = g_hooks.get_display != nullptr
                     ? g_hooks.get_display(g_hooks.user)
                     : EGL_NO_DISPLAY;
  if (d == EGL_NO_DISPLAY)
    log::w(kTag, "eglGetDisplay: acl GL device not available");
  return reinterpret_cast<jlong>(d);
}
jlong _eglCreateWindowSurface(JNIEnv* e,
                              jobject,
                              jobject display,
                              jobject config,
                              jobject native_window,
                              jintArray attrib_list) {
  if (display == nullptr || config == nullptr) {
    throw_iae(e, "eglCreateWindowSurface");
    return 0;
  }
  if (native_window == nullptr) {
    throw_iae(e, "eglCreateWindowSurface: null Surface");
    return 0;
  }
  if (g_hooks.create_window_surface == nullptr) {
    log::w(kTag,
           "eglCreateWindowSurface: no acl Surface->wl_surface hook (Stage 3)");
    return reinterpret_cast<jlong>(EGL_NO_SURFACE);
  }
  const std::vector<EGLint> a = attribs_of(e, attrib_list);
  EGLSurface s = g_hooks.create_window_surface(
      e, native_window, get_config(e, config), a.data(), g_hooks.user);
  return reinterpret_cast<jlong>(s);
}

// ── passthroughs to host Mesa libEGL
// ──────────────────────────────────────────
jboolean _eglInitialize(JNIEnv* e,
                        jobject,
                        jobject display,
                        jintArray major_minor) {
  if (display == nullptr) {
    throw_iae(e, "eglInitialize");
    return JNI_FALSE;
  }
  EGLint major = 0, minor = 0;
  const EGLBoolean ok = eglInitialize(get_display(e, display), &major, &minor);
  if (ok && major_minor != nullptr && e->GetArrayLength(major_minor) >= 2) {
    const jint mm[2] = {major, minor};
    e->SetIntArrayRegion(major_minor, 0, 2, mm);
  }
  return to_jbool(ok);
}
// The surfaces create_offscreen_surface made (see _eglCreatePbufferSurface).
// eglMakeCurrent binds one of these where the app's native GL finds it.
std::mutex g_offscreen_mutex;
std::vector<EGLSurface> g_offscreen_surfaces;
void remember_offscreen_surface(EGLSurface s) {
  if (s == EGL_NO_SURFACE)
    return;
  const std::lock_guard<std::mutex> lock(g_offscreen_mutex);
  g_offscreen_surfaces.push_back(s);
}
void forget_offscreen_surface(EGLSurface s) {
  const std::lock_guard<std::mutex> lock(g_offscreen_mutex);
  std::erase(g_offscreen_surfaces, s);
}
bool is_offscreen_surface(EGLSurface s) {
  if (s == EGL_NO_SURFACE)
    return false;
  const std::lock_guard<std::mutex> lock(g_offscreen_mutex);
  return std::find(g_offscreen_surfaces.begin(), g_offscreen_surfaces.end(),
                   s) != g_offscreen_surfaces.end();
}

jboolean _eglTerminate(JNIEnv* e, jobject, jobject display) {
  // acl owns the display's lifetime (the GlDevice) — the app terminating it
  // would tear the device down under acl. Treat as a no-op success; the device
  // is released with the run.
  (void)e;
  (void)display;
  return JNI_TRUE;
}
jboolean _eglChooseConfig(JNIEnv* e,
                          jobject,
                          jobject display,
                          jintArray attrib_list,
                          jobjectArray configs,
                          jint config_size,
                          jintArray num_config) {
  if (display == nullptr) {
    throw_iae(e, "eglChooseConfig");
    return JNI_FALSE;
  }
  const std::vector<EGLint> a = attribs_of(e, attrib_list);
  const jint want = configs != nullptr && config_size > 0 ? config_size : 0;
  // Mesa is asked for more configs than the app wants, so that the ordering
  // below can put one a gbm window can be made for first even for an app
  // that asks for a single config.
  constexpr EGLint kAskAtLeast = 128;
  const EGLint ask = want > 0 ? std::max<EGLint>(want, kAskAtLeast) : 0;
  std::vector<EGLConfig> native(static_cast<std::size_t>(ask));
  EGLint num = 0;
  EGLBoolean ok = eglChooseConfig(get_display(e, display), a.data(),
                                  ask > 0 ? native.data() : nullptr, ask, &num);
  // An engine that renders offscreen first (Godot's, which Waze draws its map
  // with) asks for a pbuffer config. On Android one config serves a window
  // and a pbuffer; Mesa's gbm platform has window configs only. Where nothing
  // matched and a pbuffer was asked for, ask again for a window config: the
  // pbuffer the app goes on to make is an offscreen window (see
  // _eglCreatePbufferSurface).
  if (ok == EGL_TRUE && num == 0 &&
      g_hooks.create_offscreen_surface != nullptr) {
    const std::vector<int> retry =
        egl_surface_type_swapped(a.data(), EGL_PBUFFER_BIT, EGL_WINDOW_BIT);
    if (!retry.empty()) {
      ok = eglChooseConfig(get_display(e, display), retry.data(),
                           ask > 0 ? native.data() : nullptr, ask, &num);
      static std::once_flag logged;
      if (ok == EGL_TRUE && num > 0) {
        std::call_once(logged, [] {
          log::i(kTag,
                 "eglChooseConfig: no pbuffer configs on this display; "
                 "answering with a window config");
        });
      }
    }
  }
  if (ok == EGL_TRUE && configs != nullptr) {
    order_window_configs(get_display(e, display), native.data(),
                         std::min(num, ask));
    num = std::min<EGLint>(num, want);  // what the app is handed
  }
  if (num_config != nullptr)
    e->SetIntArrayRegion(num_config, 0, 1, &num);
  if (ok == EGL_TRUE && configs != nullptr) {
    for (jint i = 0; i < num && i < want; ++i) {
      jobject o = e->NewObject(g_config_class, g_config_ctor,
                               reinterpret_cast<jlong>(native[i]));
      e->SetObjectArrayElement(configs, i, o);
      e->DeleteLocalRef(o);
    }
  }
  return to_jbool(ok);
}
jboolean _eglGetConfigs(JNIEnv* e,
                        jobject,
                        jobject display,
                        jobjectArray configs,
                        jint config_size,
                        jintArray num_config) {
  if (display == nullptr) {
    throw_iae(e, "eglGetConfigs");
    return JNI_FALSE;
  }
  const jint want = configs != nullptr ? config_size : 0;
  std::vector<EGLConfig> native(static_cast<std::size_t>(want));
  EGLint num = 0;
  const EGLBoolean ok = eglGetConfigs(
      get_display(e, display), want > 0 ? native.data() : nullptr, want, &num);
  if (num_config != nullptr)
    e->SetIntArrayRegion(num_config, 0, 1, &num);
  if (ok && configs != nullptr) {
    order_window_configs(get_display(e, display), native.data(),
                         num < want ? num : want);
    for (jint i = 0; i < num && i < want; ++i) {
      jobject o = e->NewObject(g_config_class, g_config_ctor,
                               reinterpret_cast<jlong>(native[i]));
      e->SetObjectArrayElement(configs, i, o);
      e->DeleteLocalRef(o);
    }
  }
  return to_jbool(ok);
}
jboolean _eglGetConfigAttrib(JNIEnv* e,
                             jobject,
                             jobject display,
                             jobject config,
                             jint attribute,
                             jintArray value) {
  if (display == nullptr || config == nullptr || value == nullptr) {
    throw_iae(e, "eglGetConfigAttrib");
    return JNI_FALSE;
  }
  EGLint v = 0;
  const EGLBoolean ok = eglGetConfigAttrib(
      get_display(e, display), get_config(e, config), attribute, &v);
  e->SetIntArrayRegion(value, 0, 1, &v);
  return to_jbool(ok);
}
jboolean _eglQueryContext(JNIEnv* e,
                          jobject,
                          jobject display,
                          jobject context,
                          jint attribute,
                          jintArray value) {
  if (display == nullptr || context == nullptr || value == nullptr) {
    throw_iae(e, "eglQueryContext");
    return JNI_FALSE;
  }
  EGLint v = 0;
  const EGLBoolean ok = eglQueryContext(get_display(e, display),
                                        get_context(e, context), attribute, &v);
  e->SetIntArrayRegion(value, 0, 1, &v);
  return to_jbool(ok);
}
jboolean _eglQuerySurface(JNIEnv* e,
                          jobject,
                          jobject display,
                          jobject surface,
                          jint attribute,
                          jintArray value) {
  if (display == nullptr || surface == nullptr || value == nullptr) {
    throw_iae(e, "eglQuerySurface");
    return JNI_FALSE;
  }
  EGLint v = 0;
  const EGLBoolean ok = eglQuerySurface(get_display(e, display),
                                        get_surface(e, surface), attribute, &v);
  e->SetIntArrayRegion(value, 0, 1, &v);
  return to_jbool(ok);
}
jlong _eglCreateContext(JNIEnv* e,
                        jobject,
                        jobject display,
                        jobject config,
                        jobject share_context,
                        jintArray attrib_list) {
  if (display == nullptr || config == nullptr) {
    throw_iae(e, "eglCreateContext");
    return 0;
  }
  const std::vector<EGLint> a = attribs_of(e, attrib_list);
  EGLContext ctx =
      eglCreateContext(get_display(e, display), get_config(e, config),
                       get_context(e, share_context), a.data());
  return reinterpret_cast<jlong>(ctx);
}
jlong _eglCreatePbufferSurface(JNIEnv* e,
                               jobject,
                               jobject display,
                               jobject config,
                               jintArray attrib_list) {
  if (display == nullptr || config == nullptr) {
    throw_iae(e, "eglCreatePbufferSurface");
    return 0;
  }
  const std::vector<EGLint> a = attribs_of(e, attrib_list);
  // A config that cannot make a pbuffer is one eglChooseConfig answered a
  // pbuffer request with (above): the surface is an offscreen window.
  EGLint surface_type = 0;
  if (g_hooks.create_offscreen_surface != nullptr &&
      eglGetConfigAttrib(get_display(e, display), get_config(e, config),
                         EGL_SURFACE_TYPE, &surface_type) == EGL_TRUE &&
      (surface_type & EGL_PBUFFER_BIT) == 0) {
    // A pbuffer's size defaults to 0 x 0; a window has to have some.
    int width = 1, height = 1;
    for (std::size_t i = 0; i + 1 < a.size() && a[i] != EGL_NONE; i += 2) {
      if (a[i] == EGL_WIDTH && a[i + 1] > 0)
        width = a[i + 1];
      else if (a[i] == EGL_HEIGHT && a[i + 1] > 0)
        height = a[i + 1];
    }
    const EGLSurface offscreen = g_hooks.create_offscreen_surface(
        get_display(e, display), get_config(e, config), width, height,
        g_hooks.user);
    remember_offscreen_surface(offscreen);
    return reinterpret_cast<jlong>(offscreen);
  }
  EGLSurface s = eglCreatePbufferSurface(get_display(e, display),
                                         get_config(e, config), a.data());
  return reinterpret_cast<jlong>(s);
}
void _eglCreatePixmapSurface(JNIEnv* e,
                             jobject,
                             jobject,
                             jobject,
                             jobject,
                             jobject,
                             jintArray) {
  if (jclass c = e->FindClass("java/lang/UnsupportedOperationException");
      c != nullptr)
    e->ThrowNew(c, "eglCreatePixmapSurface");
}
jlong _eglCreateWindowSurfaceTexture(JNIEnv* e,
                                     jobject,
                                     jobject display,
                                     jobject config,
                                     jobject surface_texture,
                                     jintArray attrib_list) {
  // SurfaceTexture-backed EGL (TextureView): the app's GLThread renders into an
  // offscreen, dma-buf-exporting GL window that acl owns; produced frames feed
  // the SurfaceTexture consumer.
  if (display == nullptr || config == nullptr) {
    throw_iae(e, "eglCreateWindowSurfaceTexture");
    return 0;
  }
  if (surface_texture == nullptr) {
    throw_iae(e, "eglCreateWindowSurfaceTexture: null SurfaceTexture");
    return 0;
  }
  if (g_hooks.create_surfacetexture_surface == nullptr) {
    log::w(kTag,
           "eglCreateWindowSurfaceTexture: no acl SurfaceTexture->GL hook");
    return reinterpret_cast<jlong>(EGL_NO_SURFACE);
  }
  const std::vector<EGLint> a = attribs_of(e, attrib_list);
  EGLSurface s = g_hooks.create_surfacetexture_surface(
      e, surface_texture, get_config(e, config), a.data(), g_hooks.user);
  return reinterpret_cast<jlong>(s);
}
// Mixed Java-EGL/native-GL bridging: run a Java-side GL Mesa call under the
// thread's native-GL donor %fs (defined in bionic_runtime.cpp, the
// -fno-stack-protector TU).
extern "C" void acl_gl_run_on_donor(void (*fn)(void*), void* arg);
extern "C" void acl_gl_donor_route_set(int on);
extern "C" int acl_gl_donor_route_get(void);
struct AclMakeCurrentArgs {
  EGLDisplay dpy;
  EGLSurface draw;
  EGLSurface read;
  EGLContext ctx;
  EGLBoolean ok;
};
struct AclSwapArgs {
  EGLDisplay dpy;
  EGLSurface surface;
  EGLBoolean ok;
};
jboolean _eglMakeCurrent(JNIEnv* e,
                         jobject,
                         jobject display,
                         jobject draw,
                         jobject read,
                         jobject context) {
  EGLDisplay dpy = get_display(e, display);
  EGLSurface d = get_surface(e, draw);
  EGLSurface r = get_surface(e, read);
  EGLContext c = get_context(e, context);
  // A SurfaceTexture producer surface: the app renders it in NATIVE GL (on the
  // bionic-%fs donor), so bind the context on THAT donor %fs — else the native
  // GL has no current context. Mark the thread donor-routed so GLUtils uploads
  // textures on the same donor. The same for an offscreen surface made for a
  // pbuffer: an engine that sets EGL up here and then draws in native code
  // (Godot in Waze) otherwise compiles its shaders with no context. And a
  // thread that is routed stays routed, so that a later bind or unbind
  // replaces the context where native GL looks for it.
  const bool st_surface = g_hooks.is_surfacetexture_surface != nullptr &&
                          d != EGL_NO_SURFACE &&
                          g_hooks.is_surfacetexture_surface(d, g_hooks.user);
  if (st_surface || is_offscreen_surface(d) || acl_gl_donor_route_get() != 0) {
    AclMakeCurrentArgs a{dpy, d, r, c, EGL_FALSE};
    acl_gl_run_on_donor(
        +[](void* p) {
          auto* a = static_cast<AclMakeCurrentArgs*>(p);
          a->ok = eglMakeCurrent(a->dpy, a->draw, a->read, a->ctx);
        },
        &a);
    // Once per thread: each call that sets it takes a slot of the table.
    if (a.ok == EGL_TRUE && acl_gl_donor_route_get() == 0)
      acl_gl_donor_route_set(1);
    return to_jbool(a.ok);
  }
  return to_jbool(eglMakeCurrent(dpy, d, r, c));
}
jboolean _eglSwapBuffers(JNIEnv* e, jobject, jobject display, jobject surface) {
  EGLSurface s = get_surface(e, surface);
  // A TextureView SurfaceTexture producer window: read back + route the frame
  // to the consumer + swap, inside the hook (the hook runs the eglSwapBuffers
  // itself on the app's native-GL donor %fs).
  if (g_hooks.swap_surfacetexture != nullptr &&
      g_hooks.swap_surfacetexture(s, g_hooks.user))
    return JNI_TRUE;
  // On a thread whose context is bound on the donor, the swap has to be made
  // there too: Mesa swaps the surface that is current where it is called.
  if (acl_gl_donor_route_get() != 0) {
    AclSwapArgs a{get_display(e, display), s, EGL_FALSE};
    acl_gl_run_on_donor(
        +[](void* p) {
          auto* a = static_cast<AclSwapArgs*>(p);
          a->ok = eglSwapBuffers(a->dpy, a->surface);
        },
        &a);
    return to_jbool(a.ok);
  }
  // Mesa's Wayland WSI attaches/commits/throttles acl's wl_surface here.
  return to_jbool(eglSwapBuffers(get_display(e, display), s));
}
jboolean _eglDestroyContext(JNIEnv* e,
                            jobject,
                            jobject display,
                            jobject context) {
  return to_jbool(
      eglDestroyContext(get_display(e, display), get_context(e, context)));
}
jboolean _eglDestroySurface(JNIEnv* e,
                            jobject,
                            jobject display,
                            jobject surface) {
  forget_offscreen_surface(get_surface(e, surface));
  if (g_hooks.surface_destroyed != nullptr)
    g_hooks.surface_destroyed(get_surface(e, surface), g_hooks.user);
  return to_jbool(
      eglDestroySurface(get_display(e, display), get_surface(e, surface)));
}
jstring _eglQueryString(JNIEnv* e, jobject, jobject display, jint name) {
  const char* s = eglQueryString(get_display(e, display), name);
  return e->NewStringUTF(s != nullptr ? s : "");
}
jint _eglGetError(JNIEnv*, jobject) {
  return static_cast<jint>(eglGetError());
}
jboolean _eglReleaseThread(JNIEnv*, jobject) {
  return to_jbool(eglReleaseThread());
}
jboolean _eglWaitGL(JNIEnv*, jobject) {
  return to_jbool(eglWaitGL());
}
jboolean _eglWaitNative(JNIEnv*, jobject, jint /*engine*/, jobject) {
  return to_jbool(eglWaitNative(EGL_CORE_NATIVE_ENGINE));
}
jboolean _eglCopyBuffers(JNIEnv* e, jobject, jobject, jobject, jobject) {
  // No native pixmap target on host — report failure via EGL_BAD_MATCH
  // semantics.
  (void)e;
  return JNI_FALSE;
}
jlong _eglGetCurrentContext(JNIEnv*, jobject) {
  return reinterpret_cast<jlong>(eglGetCurrentContext());
}
jlong _eglGetCurrentDisplay(JNIEnv*, jobject) {
  return reinterpret_cast<jlong>(eglGetCurrentDisplay());
}
jlong _eglGetCurrentSurface(JNIEnv*, jobject, jint readdraw) {
  return reinterpret_cast<jlong>(eglGetCurrentSurface(readdraw));
}
jint _getInitCount(JNIEnv*, jclass, jobject) {
  return 1;
}  // acl keeps the display initialized

#define DPY "Ljavax/microedition/khronos/egl/EGLDisplay;"
#define CTX "Ljavax/microedition/khronos/egl/EGLContext;"
#define CFG "Ljavax/microedition/khronos/egl/EGLConfig;"
#define SUR "Ljavax/microedition/khronos/egl/EGLSurface;"
#define OBJ "Ljava/lang/Object;"

const JNINativeMethod kMethods[] = {
    {"_nativeClassInit", "()V", reinterpret_cast<void*>(nativeClassInit)},
    {"eglInitialize", "(" DPY "[I)Z", reinterpret_cast<void*>(_eglInitialize)},
    {"eglQueryContext", "(" DPY CTX "I[I)Z",
     reinterpret_cast<void*>(_eglQueryContext)},
    {"eglQuerySurface", "(" DPY SUR "I[I)Z",
     reinterpret_cast<void*>(_eglQuerySurface)},
    {"eglReleaseThread", "()Z", reinterpret_cast<void*>(_eglReleaseThread)},
    {"eglChooseConfig", "(" DPY "[I[" CFG "I[I)Z",
     reinterpret_cast<void*>(_eglChooseConfig)},
    {"eglGetConfigAttrib", "(" DPY CFG "I[I)Z",
     reinterpret_cast<void*>(_eglGetConfigAttrib)},
    {"eglGetConfigs", "(" DPY "[" CFG "I[I)Z",
     reinterpret_cast<void*>(_eglGetConfigs)},
    {"eglGetError", "()I", reinterpret_cast<void*>(_eglGetError)},
    {"eglDestroyContext", "(" DPY CTX ")Z",
     reinterpret_cast<void*>(_eglDestroyContext)},
    {"eglDestroySurface", "(" DPY SUR ")Z",
     reinterpret_cast<void*>(_eglDestroySurface)},
    {"eglMakeCurrent", "(" DPY SUR SUR CTX ")Z",
     reinterpret_cast<void*>(_eglMakeCurrent)},
    {"eglQueryString", "(" DPY "I)Ljava/lang/String;",
     reinterpret_cast<void*>(_eglQueryString)},
    {"eglSwapBuffers", "(" DPY SUR ")Z",
     reinterpret_cast<void*>(_eglSwapBuffers)},
    {"eglTerminate", "(" DPY ")Z", reinterpret_cast<void*>(_eglTerminate)},
    {"eglCopyBuffers", "(" DPY SUR OBJ ")Z",
     reinterpret_cast<void*>(_eglCopyBuffers)},
    {"eglWaitGL", "()Z", reinterpret_cast<void*>(_eglWaitGL)},
    {"eglWaitNative", "(I" OBJ ")Z", reinterpret_cast<void*>(_eglWaitNative)},
    {"getInitCount", "(" DPY ")I", reinterpret_cast<void*>(_getInitCount)},
    {"_eglCreateContext", "(" DPY CFG CTX "[I)J",
     reinterpret_cast<void*>(_eglCreateContext)},
    {"_eglCreatePbufferSurface", "(" DPY CFG "[I)J",
     reinterpret_cast<void*>(_eglCreatePbufferSurface)},
    {"_eglCreatePixmapSurface", "(" SUR DPY CFG OBJ "[I)V",
     reinterpret_cast<void*>(_eglCreatePixmapSurface)},
    {"_eglCreateWindowSurface", "(" DPY CFG OBJ "[I)J",
     reinterpret_cast<void*>(_eglCreateWindowSurface)},
    {"_eglCreateWindowSurfaceTexture", "(" DPY CFG OBJ "[I)J",
     reinterpret_cast<void*>(_eglCreateWindowSurfaceTexture)},
    {"_eglGetDisplay", "(" OBJ ")J", reinterpret_cast<void*>(_eglGetDisplay)},
    {"_eglGetCurrentContext", "()J",
     reinterpret_cast<void*>(_eglGetCurrentContext)},
    {"_eglGetCurrentDisplay", "()J",
     reinterpret_cast<void*>(_eglGetCurrentDisplay)},
    {"_eglGetCurrentSurface", "(I)J",
     reinterpret_cast<void*>(_eglGetCurrentSurface)},
};
}  // namespace

jint register_egl10(JNIEnv* env, const AclEgl10Hooks& hooks) {
  g_hooks = hooks;
  jclass c = env->FindClass("com/google/android/gles_jni/EGLImpl");
  if (c == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "EGLImpl class not found");
    return JNI_ERR;
  }
  const jint rc = env->RegisterNatives(
      c, kMethods, static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
  env->DeleteLocalRef(c);
  if (rc != JNI_OK) {
    log::w(kTag, "EGLImpl RegisterNatives failed (rc={})", rc);
    return rc;
  }
  log::i(kTag,
         "com.google.android.gles_jni.EGLImpl natives registered on host Mesa "
         "libEGL");
  return JNI_OK;
}

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL

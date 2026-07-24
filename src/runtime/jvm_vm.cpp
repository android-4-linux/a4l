// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/runtime.md (pending), JNI Invocation API & JNI Functions docs
// (Oracle/OpenJDK), ADR-0001. No GPL-3.0 sources.
//
// OpenJDK-backed VM (ADR-0001 fallback). Embeds HotSpot via the JNI Invocation
// API. This runs stock JVM bytecode (.class/.jar); DEX bytecode is not loaded by
// a stock JVM — a d8-reversal/dex2jar step feeds this path later. HotSpot permits
// exactly one VM per process and does not support re-creation after
// DestroyJavaVM, so tests spawn a dedicated process.

#include "runtime/jvm_vm.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <cstdlib>
#include <ctime>

#include <dlfcn.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <jni.h>
#include <jvmti.h>

#include "acl_config.h"
#if ACL_HAVE_SDBUS
#include "svc/appearance/appearance.h"
#include "svc/battery/battery_jni.h"
#include "svc/power/power_jni.h"
#include "svc/nsd/nsd_jni.h"
#include "svc/bluetooth/bt_jni.h"
#include "svc/location/location_jni.h"
#include "svc/net/connectivity_jni.h"
#include "svc/notify/notify_jni.h"
#include "svc/sensor/sensor_jni.h"
#include "svc/storage/storage_jni.h"
#endif
#if ACL_HAVE_PIPEWIRE
#include "audio/audio_jni.h"
#endif
#include "audio/audio.h"
#include "gfx/gfx.h"
#if ACL_HAVE_VULKAN
#include "gfx/gfx_vk.h"  // S3b: VkRenderTarget import_pixels_to_dmabuf
#endif
#if ACL_HAVE_GL
#include "gfx/gl_egl.h"     // GLSurfaceView: host GL device (EGLImpl get_display)
#include "gfx/ahardware_buffer.h"  // Flutter external textures: AHardwareBuffer EGLImage translation
#include "gfx/egl10_jni.h"  // GLSurfaceView: com.google.android.gles_jni.EGLImpl natives
#endif
#include "log/log.h"
#include "ndk/bionic_runtime.h"
#include "ndk/native_bind.h"
#include "ndk/webview_functor_vk.h"  // WebView AwDrawFn Vulkan functor %fs-bridge wrappers
#include "pkg/apk.h"  // AAssetManager provider: read+inflate app assets from the APK
#include "runtime/framework_natives.h"
#include "runtime/image_reader_jni.h"  // media_jni loadLibrary -> ImageReader natives (M3c)
#include "runtime/media_codec_jni.h"   // media_jni loadLibrary -> MediaCodec natives (M5)
#include "svc/svc.h"
#include "wm/present.h"
#if ACL_PROFILE_AUTOMOTIVE
#include "hosts/cal/cal.h"             // Gate G1.5b: Car App Library template host
#include "hosts/media/media_bridge.h"  // Gate G1.5a: headless media bridge (automotive)
#endif

namespace acl::runtime {
namespace {

constexpr std::string_view kTag = "acl.runtime.jvm";

// ── AAssetManager provider (libandroid's NDK AAsset* -> the app APK) ──────────────────────────
// Flutter's engine loads its Dart kernel, snapshots, and flutter_assets through the NDK
// AAssetManager. Those live in the APK; back them with acl's pkg, which owns the inflate. The
// ndk bridge (set_asset_provider) restores glibc %fs before calling in, so plain pkg/allocator use
// is safe here. The APK is opened lazily on the first asset and cached (Flutter reads a handful).
std::mutex g_asset_apk_mtx;
std::string g_asset_apk_path;
std::optional<acl::pkg::Apk> g_asset_apk;
bool g_asset_apk_tried = false;

int acl_pkg_asset_open(void* /*user*/, const char* name, void** out_buf, long* out_len) {
  if (name == nullptr || out_buf == nullptr || out_len == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_asset_apk_mtx);
  if (!g_asset_apk && !g_asset_apk_tried) {
    g_asset_apk_tried = true;
    if (!g_asset_apk_path.empty()) {
      acl::pkg::ZipError ze;
      g_asset_apk = acl::pkg::Apk::open(g_asset_apk_path, ze);
      if (!g_asset_apk)
        log::w(kTag, "AAssetManager: cannot open APK {} ({})", g_asset_apk_path, ze.message);
    }
  }
  if (!g_asset_apk) return 0;
  // AAssetManager_open names are relative to the APK's assets/ directory.
  std::string entry = "assets/";
  entry += name;
  acl::pkg::ZipError ze;
  std::optional<std::vector<std::uint8_t>> bytes = g_asset_apk->read(entry, ze);
  if (std::getenv("ACL_ASSET_DIAG") != nullptr)
    log::i(kTag, "AAsset open '{}' -> {}", entry, bytes ? std::to_string(bytes->size()) : std::string("MISS"));
  if (!bytes) return 0;  // no such asset — a normal miss (not every open hits)
  void* buf = std::malloc(bytes->empty() ? 1 : bytes->size());
  if (buf == nullptr) return 0;
  if (!bytes->empty()) std::memcpy(buf, bytes->data(), bytes->size());
  *out_buf = buf;
  *out_len = static_cast<long>(bytes->size());
  return 1;
}
void acl_pkg_asset_free(void* /*user*/, void* buf) { std::free(buf); }

#if ACL_HAVE_GL
// The single active GLSurfaceView present session (Stage 3): the wayland GL device on acl's
// wl_display, the toplevel wl_surface the app's GLThread renders GL into, and the resulting GL
// window. Populated by present_glsurfaceview when a GLSurfaceView app presents; read by the
// EGLImpl get_display / create_window_surface hooks (which run on the app's GLThread). File-scope
// so the captureless-lambda hooks in load_acl_egl10 can reach it.
struct GlPresentSession {
  std::unique_ptr<gfx::GlDevice> device;         // wayland device on acl's wl_display
  void* wl_surface = nullptr;                    // the toplevel wl_surface (borrowed)
  std::unique_ptr<gfx::GlWindow> window;         // the app's GL window (one; fullscreen first cut)
  int w = 0, h = 0;                              // the GL surface size (= the toplevel size)
  // The app's GLThread calls eglGetDisplay EARLY (during onCreate, before acl's present sets up
  // the wayland device). get_display blocks on this until present publishes the device, so the
  // app's context is created on the SAME device as its window surface (else eglMakeCurrent
  // EGL_BAD_MATCH). Published by present_glsurfaceview's configure hook.
  std::mutex mtx;
  std::condition_variable cv;
  bool ready = false;
};
GlPresentSession g_gl_session;
// The VM boot thread == the app main/UI thread. eglGetDisplay from HERE is the throwaway
// AndroidGraphics.checkGL20 probe (during onCreate, before present); it must NOT block for the
// wayland device (that only comes after onCreate) — only the app's separate GLThread blocks.
std::thread::id g_gl_main_thread;
// A GL present session created EARLY (by the native-EGL get_display hook, during the Flutter
// engine's on-main-thread GL init inside onCreate) before PresentDriver.runGl -> nGlOpen runs.
// nGlOpen adopts it (attaches the decor View) instead of creating a second presenter. See
// create_gl_session_core + the get_display hook: this breaks the onCreate<->nGlOpen deadlock the
// newer Flutter engine triggers (it builds its GL environment + surface synchronously in onCreate,
// before the SurfaceView exists). Guarded by g_pending_gl_mtx; g_present_vm is the app's JavaVM.
struct PresentSession;                        // fwd (defined below)
PresentSession* g_pending_gl_session = nullptr;
std::mutex g_pending_gl_mtx;
JavaVM* g_present_vm = nullptr;
std::string g_present_app_id;                 // the app package (window title/app-id), for the pre-created session
#endif

// Bitmap.createBitmap(w, h) — the way BOTH framework layers spell it.
//
// The REAL android.graphics.Bitmap has no 2-arg overload: it is createBitmap(w, h, Config).
// acl's shim grew a 2-arg convenience, so code written against the shim (this file) resolved it
// happily and then died on the real framework with a bare
//   NoSuchMethodError: no static method Landroid/graphics/Bitmap;.createBitmap(II)...
// which ART turns into a process abort at its next check ("No pending exception expected"),
// because a failed GetStaticMethodID leaves the error PENDING — the same trap as a failed
// FindClass. So: ask for the real signature first, fall back to the shim's, and clear after a
// failed lookup rather than leaving a live exception behind. Returns nullptr on failure, with no
// pending exception.
jobject create_bitmap(JNIEnv* env, jclass bitmap_cls, jint w, jint h) {
  if (jclass config_cls = env->FindClass("android/graphics/Bitmap$Config"); config_cls != nullptr) {
    jmethodID create = env->GetStaticMethodID(
        bitmap_cls, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jfieldID argb_f =
        create != nullptr
            ? env->GetStaticFieldID(config_cls, "ARGB_8888", "Landroid/graphics/Bitmap$Config;")
            : nullptr;
    if (create != nullptr && argb_f != nullptr) {
      jobject argb = env->GetStaticObjectField(config_cls, argb_f);
      jobject bmp = env->CallStaticObjectMethod(bitmap_cls, create, w, h, argb);
      env->DeleteLocalRef(config_cls);
      if (argb != nullptr) env->DeleteLocalRef(argb);
      if (env->ExceptionCheck() == JNI_TRUE) {
        log::w(kTag, "Bitmap.createBitmap({}x{}, ARGB_8888) threw", w, h);
        env->ExceptionDescribe();
        env->ExceptionClear();
        return nullptr;
      }
      if (bmp == nullptr) log::w(kTag, "Bitmap.createBitmap({}x{}) returned null", w, h);
      return bmp;
    }
    env->DeleteLocalRef(config_cls);
  }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();  // no real API here: try the shim
  jmethodID create2 = env->GetStaticMethodID(bitmap_cls, "createBitmap", "(II)Landroid/graphics/Bitmap;");
  if (create2 == nullptr) {
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    log::w(kTag, "Bitmap.createBitmap: neither (II,Config) nor acl's (II) is on this classpath");
    return nullptr;
  }
  jobject bmp = env->CallStaticObjectMethod(bitmap_cls, create2, w, h);
  if (env->ExceptionCheck() == JNI_TRUE) {
    log::w(kTag, "Bitmap.createBitmap({}x{}) [acl shim 2-arg] threw", w, h);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return nullptr;
  }
  return bmp;
}

// FindClass for a class acl can do without — its own org.acl.internal.* helpers, which are absent
// whenever an app runs on the REAL framework boot classpath rather than acl's staged one.
//
// A failed FindClass leaves a pending ClassNotFoundException behind. ART is not forgiving about
// that: the next time it checks (runtime.cc "No pending exception expected") it aborts the process,
// far from the lookup that caused it. So an optional lookup must always clear. Returns nullptr when
// the class is absent, with no pending exception either way.
jclass find_class_optional(JNIEnv* env, const char* name) {
  jclass c = env->FindClass(name);
  if (c == nullptr && env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  return c;
}

// ── NDK bionic native-library loading (ADR-5) ─────────────────────────────────────────────────
// Opt-in via ACL_BIONIC_LINKER/_SYSROOT/_STUB: route the app's bionic lib/<abi>/*.so through the
// in-process bionic loader (src/ndk/bionic_runtime), which drives their JNI_OnLoad across the two-
// libc %fs boundary. acl's own glibc supplemental libs (e.g. soundpool) still load via dlopen +
// JNI_OnLoad. When the env is unset this is a no-op and ART's own nativeLoad is left in place.
#if ACL_HAVE_JNI
acl::ndk::BionicRuntime* g_ndk_rt = nullptr;
JavaVM* g_ndk_vm = nullptr;
acl::ndk::BionicConfig g_ndk_cfg;   // validated at boot; the linker bootstraps lazily from it
bool g_ndk_have_cfg = false;

// Bootstrap the bionic linker on FIRST use, not at ART boot. Bootstrapping has process-global side
// effects — it briefly maps bionic libc and touches signal state — and doing it eagerly during the
// native-floor setup, before load_system_resources, made the real libandroidfw fail to open
// framework-res.apk (every app then lost its resources). Deferring to the first actual bionic .so
// runs it after the framework is up, on the app thread that needs it. Returns g_ndk_rt or nullptr.
acl::ndk::BionicRuntime* ensure_ndk_rt() {
  if (g_ndk_rt != nullptr) return g_ndk_rt;
  if (!g_ndk_have_cfg) return nullptr;
  std::string why;
  acl::ndk::BionicRuntime* rt = acl::ndk::BionicRuntime::instance(g_ndk_cfg, &why);
  if (rt == nullptr) { log::w(kTag, "NDK bionic loader: bootstrap failed: {}", why); return nullptr; }
  g_ndk_rt = rt;
  log::i(kTag, "NDK bionic linker bootstrapped (first native lib)");
  return rt;
}

// Override of java.lang.Runtime.nativeLoad(String absPath, ClassLoader, Class caller): returns null
// on success, an error String otherwise (the libcore contract). `caller` is the class whose
// System.loadLibrary triggered the load (libcore Runtime.loadLibrary0's fromClass); it is null on
// the 2-arg nativeLoad(filename, loader) overload.
jstring JNICALL ndk_native_load(JNIEnv* env, jclass, jstring jpath, jobject loader, jobject caller) {
  const char* c = jpath ? env->GetStringUTFChars(jpath, nullptr) : nullptr;
  const std::string path = c ? c : "";
  if (c) env->ReleaseStringUTFChars(jpath, c);
  auto err = [&](const std::string& m) { return env->NewStringUTF(m.c_str()); };
  if (path.empty()) return err("acl: empty library path");

  if (acl::ndk::is_bionic_elf(path.c_str())) {
    acl::ndk::BionicRuntime* rt = ensure_ndk_rt();  // lazy bootstrap (see ensure_ndk_rt)
    if (rt == nullptr) return err("acl-bionic: loader unavailable (bootstrap failed)");
    // Give the wrapped FindClass the app's class loader so the lib's JNI_OnLoad can resolve app
    // classes (e.g. io/flutter/embedding/engine/FlutterJNI) that live in the app loader, not the
    // boot loader FindClass otherwise searches. ART's own LoadNativeLibrary does this via a
    // class-loader override; here `loader` is Runtime.nativeLoad's ClassLoader arg.
    rt->set_app_class_loader(env, loader);
    std::string why;
    void* h = rt->load_library(path.c_str(), g_ndk_vm, env, &why);
    if (h == nullptr) return err("acl-bionic: " + why);
    log::i(kTag, "loaded bionic native lib {}", path);
    // JNI_OnLoad's RegisterNatives (driven by load_library) binds what the lib chose to register.
    // Natives ART would otherwise bind by dlsym'ing Java_pkg_Class_method can't resolve at all —
    // the lib lives in the bionic linker's namespace, so ART has no handle for it — so bind them
    // here, eagerly. bind_lib_natives walks the .so's exported Java_* symbols and binds EVERY class
    // it provides natives for (libGDX-style: one .so, natives across Gdx2DPixmap/BufferUtils/…),
    // not just the loading class. See ndk/native_bind.h.
    int bound = acl::ndk::bind_lib_natives(*rt, h, env, loader, path.c_str());
    if (bound == 0) {
      // A boot-classpath lib (null loader) or a .so with no by-name exports — fall back to binding
      // just the class that asked for the library.
      if (jclass k = acl::ndk::loading_class(env, caller, loader)) {
        bound = acl::ndk::bind_class_natives(*rt, h, env, k);
        env->DeleteLocalRef(k);
      }
    }
    if (bound > 0) log::i(kTag, "bound {} native(s) by name across the lib's classes", bound);
    return nullptr;
  }
#if ACL_HAVE_GL
  // acl supplements framework native libs the host build omits. android.media.ImageReader's <clinit>
  // does loadLibrary("media_jni") then nativeClassInit(); there is no host libmedia_jni.so, so
  // satisfy the load by registering acl's ImageReader natives (over gfx::BufferQueue) and reporting
  // success — exactly what the real lib's JNI_OnLoad would do, before <clinit> calls nativeClassInit.
  if (path.find("libmedia_jni.so") != std::string::npos) {
    register_image_reader_natives(env);
    register_media_codec_natives(env);       // MediaCodec sync decode -> Surface over CodecDecoder (M5)
    register_media_codec_list_natives(env);  // MediaCodecList: advertise acl's HW decoder (ExoPlayer gate)
    log::i(kTag, "loadLibrary(media_jni): registered acl ImageReader + MediaCodec + MediaCodecList natives");
    return nullptr;
  }
#endif
  // Not a bionic app lib: a glibc/system lib (acl's own soundpool, or an ART system lib like
  // conscrypt's libjavacrypto that a boot class loads). ART resolves a bare library NAME within its
  // own namespaces' search paths; our override replaced that, so a plain dlopen of the bare name
  // fails ("cannot open shared object file"), which silently broke conscrypt — and thus APK
  // signature verification and framework-res — for every real app. Replicate the one search we
  // need: if the bare name isn't on the glibc path, look in ART's own lib dir ($ACL_ART_HOME/lib64),
  // where those system libs live (their $ORIGIN-relative NEEDED then resolve from the same dir).
  void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (h == nullptr && path.find('/') == std::string::npos) {
    if (const char* home = std::getenv("ACL_ART_HOME"); home != nullptr && *home != '\0') {
      const std::string full = std::string(home) + "/lib64/" + path;
      h = ::dlopen(full.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }
  }
  if (h == nullptr) { const char* e = ::dlerror(); return err(e ? e : "dlopen failed"); }
  if (void* onload = ::dlsym(h, "JNI_OnLoad"))
    reinterpret_cast<jint (*)(JavaVM*, void*)>(onload)(g_ndk_vm, nullptr);
  return nullptr;
}
#endif  // ACL_HAVE_JNI

// ── JVMTI diagnostic (ACL_JVMTI_DIAG) ─────────────────────────────────────────────────────────
// ART strips the class name/stack from boot-classloader NoClassDefFoundErrors, so an app-swallowed
// one is opaque. This JVMTI Exception hook names the exact throwing Java method + bytecode location
// (the site that referenced the missing class), which -verbose:class cannot. Opt-in; needs the
// -Xplugin:libopenjdkjvmti[d].so option (added below when the env is set).
void JNICALL acl_jvmti_exception(jvmtiEnv* jvmti, JNIEnv* jni, jthread, jmethodID method,
                                 jlocation location, jobject exception, jmethodID, jlocation) {
  if (exception == nullptr) return;
  jclass exc_cls = jni->GetObjectClass(exception);
  char* exc_sig = nullptr;
  if (jvmti->GetClassSignature(exc_cls, &exc_sig, nullptr) != JVMTI_ERROR_NONE || exc_sig == nullptr) {
    jni->DeleteLocalRef(exc_cls);
    return;
  }
  // The throwing method's declaring class — logged, and also used to surface exceptions thrown
  // from the window-add path (ViewRootImpl.setView's original RuntimeException is masked when
  // WindowManagerGlobal.addView's catch rolls back and the rollback itself throws).
  char *mname = nullptr, *dsig = nullptr;
  jclass decl = nullptr;
  jvmti->GetMethodName(method, &mname, nullptr, nullptr);
  if (jvmti->GetMethodDeclaringClass(method, &decl) == JVMTI_ERROR_NONE && decl != nullptr)
    jvmti->GetClassSignature(decl, &dsig, nullptr);
  // Log the opaque class-loader errors AND anything thrown from the window-add machinery (so the
  // masked original setView failure is named at throw time, before the rollback discards it).
  // ACL_JVMTI_ALL=1 widens the net to every throw except the noisy benign ClassNotFoundException
  // (LayoutInflater prefix probing) — used to catch a RuntimeException masked by a later rollback.
  const bool log_all = [] { const char* v = std::getenv("ACL_JVMTI_ALL"); return v != nullptr && *v != '\0'; }();
  const bool interesting = std::strstr(exc_sig, "NoClassDefFoundError") != nullptr ||
                           std::strstr(exc_sig, "ExceptionInInitializerError") != nullptr ||
                           (!log_all && std::strstr(exc_sig, "ClassNotFoundException") != nullptr) ||
                           (log_all && std::strstr(exc_sig, "ClassNotFoundException") == nullptr) ||
                           (dsig != nullptr && (std::strstr(dsig, "ViewRootImpl") != nullptr ||
                                                std::strstr(dsig, "WindowManagerGlobal") != nullptr));
  if (interesting) {
    // Throwable.detailMessage names the class for ClassNotFoundException (and holds the stripped
    // text for the boot NoClassDefFoundError). Read the field directly — GetObjectField is safe with
    // an exception pending (it invokes no Java), unlike CallObjectMethod(getMessage).
    std::string msg;
    if (jclass thr = jni->FindClass("java/lang/Throwable")) {
      if (jfieldID dm = jni->GetFieldID(thr, "detailMessage", "Ljava/lang/String;")) {
        if (auto s = static_cast<jstring>(jni->GetObjectField(exception, dm))) {
          if (const char* c = jni->GetStringUTFChars(s, nullptr)) { msg = c; jni->ReleaseStringUTFChars(s, c); }
          jni->DeleteLocalRef(s);
        }
      }
      jni->DeleteLocalRef(thr);
    }
    log::w(kTag, "JVMTI: {} msg='{}' at {}.{} loc={}", exc_sig, msg, dsig ? dsig : "?",
           mname ? mname : "?", static_cast<long long>(location));
  }
  if (mname) jvmti->Deallocate(reinterpret_cast<unsigned char*>(mname));
  if (dsig) jvmti->Deallocate(reinterpret_cast<unsigned char*>(dsig));
  if (decl) jni->DeleteLocalRef(decl);
  jvmti->Deallocate(reinterpret_cast<unsigned char*>(exc_sig));
  jni->DeleteLocalRef(exc_cls);
}

// ACL_JVMTI_TRACE: breakpoint-log key Flutter-embedding startup methods to see how far the engine
// gets (does executeDartEntrypoint run?). Breakpoints only fire for the named methods, so this is
// cheap (unlike global MethodEntry). Set from a ClassPrepare hook as the classes load.
void JNICALL acl_jvmti_breakpoint(jvmtiEnv* jvmti, JNIEnv*, jthread thread, jmethodID method,
                                  jlocation) {
  char* mname = nullptr; char* dsig = nullptr; jclass decl = nullptr;
  jvmti->GetMethodName(method, &mname, nullptr, nullptr);
  if (jvmti->GetMethodDeclaringClass(method, &decl) == JVMTI_ERROR_NONE && decl != nullptr)
    jvmti->GetClassSignature(decl, &dsig, nullptr);
  log::i(kTag, "JVMTI-TRACE: entered {}.{}", dsig ? dsig : "?", mname ? mname : "?");
  // For Runtime.exit/halt (ACL_TRACE_EXIT), dump WHO is exiting — a clean System.exit(0) during
  // startup is otherwise anonymous (ART logs the status, not the caller).
  const bool is_exit = mname != nullptr && (std::strcmp(mname, "exit") == 0 ||
                                            std::strcmp(mname, "halt") == 0);
  if (is_exit) {
    constexpr jint kMax = 32;
    jvmtiFrameInfo frames[kMax];
    jint depth = 0;
    bool from_uncaught = false;
    if (jvmti->GetStackTrace(thread, 0, kMax, frames, &depth) == JVMTI_ERROR_NONE) {
      log::i(kTag, "JVMTI-TRACE: exit caller stack ({} frames):", depth);
      for (jint f = 0; f < depth; ++f) {
        char* fn = nullptr; char* fd = nullptr; jclass fc = nullptr;
        jvmti->GetMethodName(frames[f].method, &fn, nullptr, nullptr);
        if (jvmti->GetMethodDeclaringClass(frames[f].method, &fc) == JVMTI_ERROR_NONE && fc != nullptr)
          jvmti->GetClassSignature(fc, &fd, nullptr);
        // An exit dispatched from an uncaught-exception handler is a crash-handler self-kill: many
        // apps (Pandora's StatsUncaughtExceptionHandler) install a default handler that
        // System.exit(0)s on ANY uncaught background exception, so one analytics-SDK exception in a
        // worker takes the whole process down before the UI. That is the pattern ACL_SUPPRESS_EXIT
        // neutralizes -- NOT a deliberate quit (a Quit button's exit has no uncaughtException frame).
        if (fn != nullptr && std::strcmp(fn, "uncaughtException") == 0) from_uncaught = true;
        log::i(kTag, "JVMTI-TRACE:   #{} {}{}", f, fd ? fd : "?", fn ? fn : "?");
        if (fn) jvmti->Deallocate(reinterpret_cast<unsigned char*>(fn));
        if (fd) jvmti->Deallocate(reinterpret_cast<unsigned char*>(fd));
      }
    }
    // ACL_SUPPRESS_EXIT: abort a crash-handler self-kill by forcing Runtime.exit/halt to return
    // early (the process keeps running; the offending worker thread just dies with its exception
    // logged). Opt-in and scoped to uncaught-handler exits so ordinary System.exit() still works.
    static const bool suppress = [] {
      const char* v = std::getenv("ACL_SUPPRESS_EXIT");
      return v != nullptr && *v != '\0' && *v != '0';
    }();
    if (suppress && from_uncaught) {
      if (jvmti->ForceEarlyReturnVoid(thread) == JVMTI_ERROR_NONE)
        log::w(kTag, "JVMTI: suppressed crash-handler System.exit (ACL_SUPPRESS_EXIT); process continues");
      else
        log::w(kTag, "JVMTI: ForceEarlyReturnVoid failed — exit not suppressed");
    }
  }
  if (mname) jvmti->Deallocate(reinterpret_cast<unsigned char*>(mname));
  if (dsig) jvmti->Deallocate(reinterpret_cast<unsigned char*>(dsig));
}
void JNICALL acl_jvmti_class_prepare(jvmtiEnv* jvmti, JNIEnv*, jthread, jclass klass) {
  char* sig = nullptr;
  if (jvmti->GetClassSignature(klass, &sig, nullptr) != JVMTI_ERROR_NONE || sig == nullptr) return;
  const bool interesting = std::strstr(sig, "engine/FlutterJNI") != nullptr;
  if (interesting) {
    int set = 0;
    jint n = 0; jmethodID* ms = nullptr;
    if (jvmti->GetClassMethods(klass, &n, &ms) == JVMTI_ERROR_NONE) {
      for (jint i = 0; i < n; ++i) {
        char* mn = nullptr;
        if (jvmti->GetMethodName(ms[i], &mn, nullptr, nullptr) == JVMTI_ERROR_NONE && mn != nullptr) {
          if (std::strstr(mn, "runBundle") || std::strstr(mn, "PlatformMessage") ||
              std::strstr(mn, "ViewportMetrics") || std::strstr(mn, "spawn") ||
              std::strstr(mn, "onFirstFrame") || std::strstr(mn, "Vsync"))
            if (jvmti->SetBreakpoint(ms[i], 0) == JVMTI_ERROR_NONE) { ++set;
              log::i(kTag, "JVMTI-TRACE:   bp on {}", mn); }
          jvmti->Deallocate(reinterpret_cast<unsigned char*>(mn));
        }
      }
      jvmti->Deallocate(reinterpret_cast<unsigned char*>(ms));
    }
    log::i(kTag, "JVMTI-TRACE: prepared {} ({} breakpoints)", sig, set);
  }
  jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
}

// ── Java-stack sampler (ACL_STALL_DIAG=<seconds>) ─────────────────────────────────────────────
// When an app renders nothing, the question is always "what is the UI thread actually doing?".
// eu-stack cannot answer it: acl runs ART interpreted, so every native stack is an unreadable
// tower of art::interpreter::ExecuteSwitchImplCpp frames with no Java identity. SIGQUIT cannot
// either — acl embeds ART with JNI_CreateJavaVM, so ART's SignalCatcher/thread-dump is never
// wired up. This samples the "main" thread's JAVA stack from a watchdog thread every N seconds
// and logs it, which names the method the UI thread is spending its time in.
void jvmti_stack_sampler(JavaVM* vm, jvmtiEnv* jvmti, int period_s) {
  JNIEnv* env = nullptr;
  JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("acl-stall-diag"), nullptr};
  if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) return;
  // Deep enough to reach the bottom of a real UI stack: a React Native draw nests one
  // ReactViewGroup per JS view, so 24 frames did not even leave the view tree.
  constexpr jint kMaxFrames = 160;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(period_s));
    jint count = 0;
    jthread* threads = nullptr;
    if (jvmti->GetAllThreads(&count, &threads) != JVMTI_ERROR_NONE || threads == nullptr) continue;
    for (jint i = 0; i < count; ++i) {
      jvmtiThreadInfo info{};
      if (jvmti->GetThreadInfo(threads[i], &info) != JVMTI_ERROR_NONE) continue;
      const bool is_main = info.name != nullptr && std::strcmp(info.name, "main") == 0;
      if (info.name != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char*>(info.name));
      if (!is_main) continue;
      jvmtiFrameInfo frames[kMaxFrames];
      jint depth = 0;
      if (jvmti->GetStackTrace(threads[i], 0, kMaxFrames, frames, &depth) != JVMTI_ERROR_NONE)
        break;
      log::i(kTag, "stall-diag: main thread Java stack ({} frames)", depth);
      for (jint f = 0; f < depth; ++f) {
        char *mname = nullptr, *dsig = nullptr;
        jclass decl = nullptr;
        jvmti->GetMethodName(frames[f].method, &mname, nullptr, nullptr);
        if (jvmti->GetMethodDeclaringClass(frames[f].method, &decl) == JVMTI_ERROR_NONE &&
            decl != nullptr)
          jvmti->GetClassSignature(decl, &dsig, nullptr);
        log::i(kTag, "stall-diag:   #{} {}{}", f, dsig != nullptr ? dsig : "?",
               mname != nullptr ? mname : "?");
        if (mname != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char*>(mname));
        if (dsig != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char*>(dsig));
        if (decl != nullptr) env->DeleteLocalRef(decl);
      }
      break;
    }
    for (jint i = 0; i < count; ++i) env->DeleteLocalRef(threads[i]);
    jvmti->Deallocate(reinterpret_cast<unsigned char*>(threads));
  }
}

// Attach a JVMTI env to the running VM and enable the Exception event. Best-effort; needs the
// JVMTI plugin loaded (-Xplugin) — GetEnv(JVMTI_VERSION) returns JNI_EVERSION otherwise.
void install_jvmti_diag(JavaVM* vm) {
  jvmtiEnv* jvmti = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK || jvmti == nullptr) {
    log::w(kTag, "JVMTI: plugin not loaded (GetEnv failed) — no exception naming");
    return;
  }
  const bool trace = [] { const char* v = std::getenv("ACL_JVMTI_TRACE"); return v != nullptr && *v != '\0'; }();
  // ACL_TRACE_EXIT: break on Runtime.exit/halt and dump the caller's Java stack, to name whoever
  // cleanly System.exit()s during startup (ART logs the status, not the caller).
  // ACL_SUPPRESS_EXIT: additionally abort crash-handler self-kills (exit from an uncaught-exception
  // handler) via ForceEarlyReturnVoid, so one background analytics-SDK exception can't take the
  // whole process down before the UI. Both use the same Runtime.exit/halt breakpoints.
  const bool trace_exit = [] {
    for (const char* v : {std::getenv("ACL_TRACE_EXIT"), std::getenv("ACL_SUPPRESS_EXIT")})
      if (v != nullptr && *v != '\0' && *v != '0') return true;
    return false;
  }();
  const bool suppress_exit = [] { const char* v = std::getenv("ACL_SUPPRESS_EXIT"); return v != nullptr && *v != '\0' && *v != '0'; }();
  jvmtiCapabilities caps{};
  caps.can_generate_exception_events = 1;
  if (trace || trace_exit) { caps.can_generate_breakpoint_events = 1; }
  if (suppress_exit) { caps.can_force_early_return = 1; }
  jvmti->AddCapabilities(&caps);
  jvmtiEventCallbacks cbs{};
  cbs.Exception = &acl_jvmti_exception;
  if (trace) { cbs.ClassPrepare = &acl_jvmti_class_prepare; }
  if (trace || trace_exit) { cbs.Breakpoint = &acl_jvmti_breakpoint; }
  jvmti->SetEventCallbacks(&cbs, sizeof(cbs));
  jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_EXCEPTION, nullptr);
  if (trace) {
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, nullptr);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_BREAKPOINT, nullptr);
    log::i(kTag, "JVMTI: Flutter startup tracing enabled (ACL_JVMTI_TRACE)");
  }
  if (trace_exit) {
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_BREAKPOINT, nullptr);
    // java.lang.Runtime is already loaded (VM init), so class_prepare won't fire for it — set the
    // breakpoints directly. System.exit -> Runtime.exit -> Runtime.halt.
    jvmtiEnv* je = jvmti;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
      if (jclass rt = env->FindClass("java/lang/Runtime")) {
        int set = 0;
        for (const char* m : {"exit", "halt"}) {
          if (jmethodID mid = env->GetMethodID(rt, m, "(I)V")) {
            if (je->SetBreakpoint(mid, 0) == JVMTI_ERROR_NONE) ++set;
          }
          env->ExceptionClear();
        }
        log::i(kTag, "JVMTI: exit {} enabled ({} breakpoints on Runtime.exit/halt)",
               suppress_exit ? "suppression" : "tracing", set);
        env->DeleteLocalRef(rt);
      }
    }
  }
  log::i(kTag, "JVMTI: exception naming enabled (ACL_JVMTI_DIAG)");
  if (const char* s = std::getenv("ACL_STALL_DIAG"); s != nullptr && *s != '\0') {
    const int period = std::max(1, std::atoi(s));
    std::thread(jvmti_stack_sampler, vm, jvmti, period).detach();
    log::i(kTag, "JVMTI: main-thread Java stack sampler every {}s (ACL_STALL_DIAG)", period);
  }
}

// acl graphics font seam: dlsym'd from libacl_graphics_jni (load_acl_graphics) — builds an
// SkTypeface* handle from a font-file path for org.acl.internal.HostBootstrap.nTypefaceFromPath
// (distinct system font families). Null until the graphics lib loads; the native returns 0 then.
jlong (*g_acl_typeface_from_path)(const char*) = nullptr;

// StrictMode thread policy slot (android.os.Binder.set/getThreadStrictModePolicy). Per-thread,
// like the real Binder thread-local; no Binder, so it just round-trips in-process.
thread_local jint g_strict_mode_policy = 0;

#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
// ── Looper-driven present (approach A): native seam for org.acl.internal.PresentDriver ──
// The Java driver owns Looper.loop() and the Choreographer; these natives expose the wm
// presenter primitives, the GPU begin/end-frame, and the vsync-delivery hooks so a real
// Android frame loop drives dma-buf submission. See docs/looper-driven-present.md.

// Mirror of libacl_graphics_jni's AclDmabuf (the GPU end-frame output descriptor).
struct PresentDmabuf {
  int fd;
  std::uint32_t width, height, plane_count;
  std::uint32_t offsets[4], strides[4];
  std::uint64_t modifier;
  std::uint32_t drm_format;
};

// Map a Linux evdev scancode to an Android KeyEvent keycode (a small practical subset;
// unmapped keys become KEYCODE_UNKNOWN=0). Mirrors Android's Generic.kl.
jint acl_evdev_to_android_keycode(std::uint32_t evdev) {
  switch (evdev) {
    case 30: return 29;  case 48: return 30;  case 46: return 31;  case 32: return 32;  // A B C D
    case 18: return 33;  case 33: return 34;  case 34: return 35;  case 35: return 36;  // E F G H
    case 23: return 37;  case 36: return 38;  case 37: return 39;  case 38: return 40;  // I J K L
    case 50: return 41;  case 49: return 42;  case 24: return 43;  case 25: return 44;  // M N O P
    case 16: return 45;  case 19: return 46;  case 31: return 47;  case 20: return 48;  // Q R S T
    case 22: return 49;  case 47: return 50;  case 17: return 51;  case 45: return 52;  // U V W X
    case 21: return 53;  case 44: return 54;                                            // Y Z
    case 2: return 8;   case 3: return 9;   case 4: return 10;  case 5: return 11;       // 1 2 3 4
    case 6: return 12;  case 7: return 13;  case 8: return 14;  case 9: return 15;       // 5 6 7 8
    case 10: return 16; case 11: return 7;                                               // 9 0
    case 28: return 66;  case 57: return 62;  case 1: return 111;  case 14: return 67;   // ENT SPC ESC DEL
    case 15: return 61;  case 105: return 21; case 106: return 22;                       // TAB LEFT RIGHT
    case 103: return 19; case 108: return 20;                                            // UP DOWN
    default: return 0;
  }
}

// A monotonic timestamp (ns) for MotionEvent/KeyEvent times.
std::uint64_t present_now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

// Per-window native state for PresentDriver; its address is the jlong `s` handed to Java.
struct RenderThread;  // per-window GPU render thread (deferred-present split); defined below

struct PresentSession {
  std::unique_ptr<wm::IPresenter> presenter;
  std::unique_ptr<RenderThread> rthread;  // owns all GrContext work when deferred (ACL_PRESENT_RT)
  bool deferred = false;                  // ACL_PRESENT_RT: record on UI thread, render on rthread
  bool rt_bootstrapped = false;           // first deferred frame renders sync to arm the vsync loop
  JavaVM* vm = nullptr;
  jobject view = nullptr;    // global ref to the decor View
  jobject canvas = nullptr;  // global ref to the RecordingCanvas over the GPU SkCanvas
  jobject root_node = nullptr;  // global ref to the present-root RenderNode backing `canvas`
  int w = 0, h = 0;
  jint wspec = 0, hspec = 0;  // MeasureSpec.EXACTLY(w/h)
  long (*gpu_begin)(int, int) = nullptr;
  bool (*gpu_end)(int, PresentDmabuf*) = nullptr;
  void (*deliver_vsync)(JNIEnv*, jlong) = nullptr;
  int (*vsync_pending)() = nullptr;
  void (*vsync_set_immediate)(int) = nullptr;
  jmethodID m_measure = nullptr, m_gmw = nullptr, m_gmh = nullptr, m_layout = nullptr, m_draw = nullptr;
  jclass motionevent_cls = nullptr, keyevent_cls = nullptr;  // global refs
  jmethodID me_obtain = nullptr, me_recycle = nullptr, ke_obtain = nullptr;
  jmethodID dispatch_touch = nullptr, dispatch_key = nullptr;
  jclass hostbootstrap_cls = nullptr;                 // global ref
  jmethodID route_overlay_touch = nullptr;            // HostBootstrap.routeOverlayTouch(MotionEvent)Z
  std::uint64_t down_time = 0;
  std::uint64_t frames = 0;  // presented-frame count (progress evidence)
  // Latest real wp_presentation feedback (present-clock ns / refresh ns / KIND flags), sampled on
  // the present thread (nDispatch, where OnPresented lands) and read by the RenderThread to push
  // into Chromium's WebView Viz output device — so its presentation-time callbacks / pacing get true
  // vsync timing instead of draw-time TimeTicks::Now() + a zero interval. 0 = none yet.
  std::atomic<std::uint64_t> pf_present_ns{0};
  std::atomic<std::uint32_t> pf_refresh_ns{0};
  std::atomic<std::uint32_t> pf_flags{0};
  int vsync_fd = -1;         // GL mode: timerfd clocking Choreographer vsync (Mesa owns commit)
  bool is_gl = false;        // GL mode: no Skia presenter / nTraverse; Mesa renders the toplevel
};

// Per-window RenderThread: owns ALL GrContext/Vulkan work (gpu_begin clear + present picture replay +
// gpu_end flush/copy), so the present/UI thread does zero GrContext work — GrDirectContext is
// single-threaded, and the device is created + used only on this thread. The WebView functor's
// draw_vk (Chromium HardwareRenderer::Draw) therefore executes HERE, off the UI thread — the whole
// point of the split. Step 2 runs it SYNCHRONOUSLY (present thread posts a frame, blocks for the
// dma-buf); Step 3 will run it one frame behind so the UI thread never blocks (unblocking the WebView
// producer/consumer pipeline). See webview-renderthread-fix.
struct RenderThread {
  PresentSession* s = nullptr;
  std::thread th;
  std::mutex mu;
  std::condition_variable cv_job, cv_done;
  int w = 0, h = 0, bufidx = 0;   // job params (set under mu before notify)
  PresentDmabuf out{};            // last completed frame's dma-buf
  int out_bufidx = 0;             // the ping-pong slot it was rendered into (pin submit to it)
  bool out_ok = false;
  bool has_job = false, in_progress = false, completed_new = false, quit = false;

  void start(PresentSession* sess) {
    s = sess;
    th = std::thread([this] { loop(); });
  }
  void loop() {
    // The WebView functor's draw_vk (bionic code) runs on this thread via call_bionic's RunScope,
    // which needs a bionic TCB. Create it up front (this only mints the TCB; the running %fs stays
    // glibc for Skia/Vulkan — RunScope swaps per functor call). Surfaces attach failures explicitly.
    if (auto* rt = acl::ndk::BionicRuntime::get()) {
      std::string why;
      if (rt->attach_current_thread(&why) == 0)
        acl::log::w(kTag, "RenderThread: bionic attach failed: {}", why);
      else
        acl::log::i(kTag, "RenderThread: attached to bionic");
    } else {
      acl::log::w(kTag, "RenderThread: BionicRuntime not available");
    }
    for (;;) {
      int W, H, buf;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv_job.wait(lk, [this] { return has_job || quit; });
        if (quit) return;
        has_job = false;
        in_progress = true;
        W = w; H = h; buf = bufidx;
      }
      // All GrContext work on this thread: clear (+ lazy device/GrContext creation on first call),
      // then replay the recorded present picture + flush + copy to dma-buf. The WebView functor's
      // draw_vk (HardwareRenderer::Draw, which BLOCKS on the renderer's frame) runs inside gpu_end
      // here — off the UI thread, so the UI keeps ticking the Choreographer/begin-frames.
      bool ok = false;
      PresentDmabuf f{};
      // Forward the host compositor's real wp_presentation feedback (sampled on the present thread)
      // into Chromium's WebView Viz output device on THIS thread, before the functor's draw_vk inside
      // gpu_end runs its SwapBuffers/CallFeedback — so WebView pacing/presentation callbacks get true
      // vsync timing instead of draw-time TimeTicks::Now() + a zero interval. WORK IN PROGRESS,
      // opt-in via ACL_PRESENT_FEEDBACK (the Chromium consumer is gated on the same env): the push is
      // proven to reach plat_support with real values, but enabling it currently correlates with a
      // present-loop stall under investigation, so it is off by default. See webview-renderthread-fix.
      static const bool acl_pf_enabled = std::getenv("ACL_PRESENT_FEEDBACK") != nullptr;
      if (acl_pf_enabled) {
        if (const std::uint64_t pn = s->pf_present_ns.load(std::memory_order_relaxed); pn != 0) {
          acl_webview_functor_vk_set_present_timing(
              static_cast<std::int64_t>(pn),
              static_cast<std::int64_t>(s->pf_refresh_ns.load(std::memory_order_relaxed)),
              s->pf_flags.load(std::memory_order_relaxed));
        }
      }
      if (s->gpu_begin != nullptr && s->gpu_begin(W, H) != 0 && s->gpu_end != nullptr) {
        ok = s->gpu_end(buf, &f);
      }
      {
        std::lock_guard<std::mutex> lk(mu);
        out = f;
        out_bufidx = buf;
        out_ok = ok;
        in_progress = false;
        completed_new = true;
      }
      cv_done.notify_one();
    }
  }
  // Post + block until the frame is rendered (used ONCE to bootstrap the vsync loop — keepalive is a
  // no-op until the first real submit, so the first frame must render+submit synchronously).
  bool render_sync(int W, int H, int buf, PresentDmabuf* f) {
    post(W, H, buf);
    std::unique_lock<std::mutex> lk(mu);
    cv_done.wait(lk, [this] { return completed_new; });
    completed_new = false;
    if (f != nullptr) *f = out;
    return out_ok;
  }
  // Is the RenderThread rendering (or about to)? The UI thread must NOT block on it — it skips the
  // tick (keepalive) so the Looper/Choreographer keeps driving the renderer's begin-frames.
  bool busy() {
    std::lock_guard<std::mutex> lk(mu);
    return has_job || in_progress;
  }
  // Non-blocking: hand back the last completed frame's dma-buf + the slot it was rendered into (once).
  // Returns false if none new.
  bool take_completed(PresentDmabuf* f, int* slot) {
    std::lock_guard<std::mutex> lk(mu);
    if (!completed_new) return false;
    completed_new = false;
    if (!out_ok) return false;
    if (f != nullptr) *f = out;
    if (slot != nullptr) *slot = out_bufidx;
    return true;
  }
  // Non-blocking: start rendering the just-taken present picture into `buf`. Caller must have
  // snapshotted the picture (acl_graphics_present_take_for_render) first and confirmed !busy().
  void post(int W, int H, int buf) {
    {
      std::lock_guard<std::mutex> lk(mu);
      w = W; h = H; bufidx = buf;
      has_job = true;
    }
    cv_job.notify_one();
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu);
      quit = true;
    }
    cv_job.notify_one();
    if (th.joinable()) th.join();
  }
};

// The current thread's JNIEnv (input callbacks run on the main thread during nDispatch, but
// were created in nOpen — re-fetch rather than capture a possibly-stale env).
JNIEnv* present_env(PresentSession* s) {
  JNIEnv* e = nullptr;
  if (s != nullptr && s->vm != nullptr) s->vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
  return e;
}

// Sync the compositor's text-input to the currently-focused view. Called after each input event:
// when a tap moves focus onto a text editor (EditText / Compose TextField, i.e. View
// .onCheckIsTextEditor() == true) this is a FRESH enable — which is what makes the compositor show
// its on-screen keyboard — and a disable when focus leaves text. This mirrors a real app's
// InputMethodManager show/hide, driven by actual view focus (the AOSP IMM requestImeShow gate does
// not function on acl's hollow ViewRootImpl, so acl drives it from focus directly).
void present_sync_ime_focus(PresentSession* s) {
  JNIEnv* env = present_env(s);
  if (env == nullptr || s->presenter == nullptr || s->view == nullptr) return;
  jclass vcls = env->GetObjectClass(s->view);
  jmethodID find_focus = vcls ? env->GetMethodID(vcls, "findFocus", "()Landroid/view/View;") : nullptr;
  jobject focused = find_focus ? env->CallObjectMethod(s->view, find_focus) : nullptr;
  bool want = false;
  if (focused != nullptr) {
    jclass fcls = env->GetObjectClass(focused);
    jmethodID is_editor = fcls ? env->GetMethodID(fcls, "onCheckIsTextEditor", "()Z") : nullptr;
    if (is_editor != nullptr) want = env->CallBooleanMethod(focused, is_editor) == JNI_TRUE;
    if (fcls != nullptr) env->DeleteLocalRef(fcls);
    env->DeleteLocalRef(focused);
  }
  if (vcls != nullptr) env->DeleteLocalRef(vcls);
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  s->presenter->set_ime_visible(want);  // no-ops if unchanged (dedup in the presenter)
}

// Deliver a compositor pointer/key event to the decor View (native side of MotionEvent/
// KeyEvent). Runs synchronously inside presenter->dispatch() on the main thread.
void present_deliver_input(PresentSession* s, const wm::InputEvent& ev) {
  JNIEnv* env = present_env(s);
  if (env == nullptr) return;
  if (const auto* p = std::get_if<wm::PointerInput>(&ev)) {
    if (s->motionevent_cls == nullptr || s->me_obtain == nullptr || s->dispatch_touch == nullptr) return;
    const jint action = p->action == wm::PointerInput::Action::Down ? 0
                      : p->action == wm::PointerInput::Action::Up   ? 1 : 2;
    if (action == 0) s->down_time = present_now_ns();
    jobject me = env->CallStaticObjectMethod(
        s->motionevent_cls, s->me_obtain, static_cast<jlong>(s->down_time),
        static_cast<jlong>(present_now_ns()), action, static_cast<jfloat>(p->x),
        static_cast<jfloat>(p->y), 0);
    if (me != nullptr) {
      // A Dialog/PopupWindow overlay, when present, takes input first (it is drawn on top and,
      // if modal, blocks the Activity behind). HostBootstrap.routeOverlayTouch translates the
      // event into the overlay's local space and dispatches it; if it consumes the event, the
      // Activity view is skipped. Absent an overlay it returns false and the Activity gets it.
      jboolean consumed = JNI_FALSE;
      if (s->route_overlay_touch != nullptr && s->hostbootstrap_cls != nullptr)
        consumed = env->CallStaticBooleanMethod(s->hostbootstrap_cls, s->route_overlay_touch, me);
      if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionClear(); consumed = JNI_FALSE; }
      jboolean handled = consumed;
      if (consumed == JNI_FALSE)
        handled = env->CallBooleanMethod(s->view, s->dispatch_touch, me);
      static int idiag = 0;
      if (idiag < 24) {
        log::i(kTag, "input: pointer action={} ({},{}) -> {} handled={}",
               action, p->x, p->y, consumed == JNI_TRUE ? "overlay" : "activity", handled == JNI_TRUE);
        ++idiag;
      }
      if (s->me_recycle != nullptr) env->CallVoidMethod(me, s->me_recycle);
      env->DeleteLocalRef(me);
    }
    if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }
    present_sync_ime_focus(s);  // a tap that focused a text field -> show the OSK
  } else if (const auto* k = std::get_if<wm::KeyInput>(&ev)) {
    if (s->keyevent_cls == nullptr || s->ke_obtain == nullptr || s->dispatch_key == nullptr) return;
    const jint action = k->action == wm::KeyInput::Action::Down ? 0 : 1;
    jobject ke = env->CallStaticObjectMethod(s->keyevent_cls, s->ke_obtain, action,
                                             acl_evdev_to_android_keycode(k->keycode));
    if (ke != nullptr) {
      env->CallBooleanMethod(s->view, s->dispatch_key, ke);
      env->DeleteLocalRef(ke);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    present_sync_ime_focus(s);  // a key (e.g. Tab) that moved focus onto/off a text field
  } else if (const auto* t = std::get_if<wm::TextInput>(&ev)) {
    // IME text from the compositor: route to the focused view's InputConnection. preedit ->
    // setComposingText (in-progress), a final commit -> commitText. The focused editor is resolved
    // per event (findFocus, then onCreateInputConnection) so it tracks focus changes; a Compose
    // AndroidComposeView returns the connection for its active TextField.
    log::i(kTag, "IME {}: \"{}\"", t->preedit ? "preedit" : "committed", t->text);
    jclass vcls = env->GetObjectClass(s->view);
    jmethodID find_focus = vcls ? env->GetMethodID(vcls, "findFocus", "()Landroid/view/View;") : nullptr;
    jobject focused = find_focus ? env->CallObjectMethod(s->view, find_focus) : nullptr;
    jobject target = focused != nullptr ? focused : s->view;
    jclass fcls = env->GetObjectClass(target);
    jmethodID create_ic = fcls ? env->GetMethodID(
        fcls, "onCreateInputConnection",
        "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;") : nullptr;
    jclass ei_cls = env->FindClass("android/view/inputmethod/EditorInfo");
    jmethodID ei_ctor = ei_cls ? env->GetMethodID(ei_cls, "<init>", "()V") : nullptr;
    jobject ei = ei_ctor ? env->NewObject(ei_cls, ei_ctor) : nullptr;
    jobject ic = (create_ic != nullptr && ei != nullptr) ? env->CallObjectMethod(target, create_ic, ei) : nullptr;
    if (ic != nullptr) {
      jclass ic_cls = env->GetObjectClass(ic);
      jmethodID m = ic_cls ? env->GetMethodID(ic_cls, t->preedit ? "setComposingText" : "commitText",
                                              "(Ljava/lang/CharSequence;I)Z") : nullptr;
      jstring str = env->NewStringUTF(t->text.c_str());
      if (m != nullptr) env->CallBooleanMethod(ic, m, str, static_cast<jint>(t->cursor));
      if (str != nullptr) env->DeleteLocalRef(str);
      if (ic_cls != nullptr) env->DeleteLocalRef(ic_cls);
      env->DeleteLocalRef(ic);
    }
    if (ei != nullptr) env->DeleteLocalRef(ei);
    if (ei_cls != nullptr) env->DeleteLocalRef(ei_cls);
    if (fcls != nullptr) env->DeleteLocalRef(fcls);
    if (focused != nullptr) env->DeleteLocalRef(focused);
    if (vcls != nullptr) env->DeleteLocalRef(vcls);
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  } else if (const auto* d = std::get_if<wm::DeleteSurroundingText>(&ev)) {
    log::i(kTag, "IME delete-surrounding: before={} after={}", d->before, d->after);
    jclass vcls = env->GetObjectClass(s->view);
    jmethodID find_focus = vcls ? env->GetMethodID(vcls, "findFocus", "()Landroid/view/View;") : nullptr;
    jobject focused = find_focus ? env->CallObjectMethod(s->view, find_focus) : nullptr;
    jobject target = focused != nullptr ? focused : s->view;
    jclass fcls = env->GetObjectClass(target);
    jmethodID create_ic = fcls ? env->GetMethodID(
        fcls, "onCreateInputConnection",
        "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;") : nullptr;
    jclass ei_cls = env->FindClass("android/view/inputmethod/EditorInfo");
    jmethodID ei_ctor = ei_cls ? env->GetMethodID(ei_cls, "<init>", "()V") : nullptr;
    jobject ei = ei_ctor ? env->NewObject(ei_cls, ei_ctor) : nullptr;
    jobject ic = (create_ic != nullptr && ei != nullptr) ? env->CallObjectMethod(target, create_ic, ei) : nullptr;
    if (ic != nullptr) {
      jclass ic_cls = env->GetObjectClass(ic);
      jmethodID m = ic_cls ? env->GetMethodID(ic_cls, "deleteSurroundingText", "(II)Z") : nullptr;
      if (m != nullptr) env->CallBooleanMethod(ic, m, static_cast<jint>(d->before), static_cast<jint>(d->after));
      if (ic_cls != nullptr) env->DeleteLocalRef(ic_cls);
      env->DeleteLocalRef(ic);
    }
    if (ei != nullptr) env->DeleteLocalRef(ei);
    if (ei_cls != nullptr) env->DeleteLocalRef(ei_cls);
    if (fcls != nullptr) env->DeleteLocalRef(fcls);
    if (focused != nullptr) env->DeleteLocalRef(focused);
    if (vcls != nullptr) env->DeleteLocalRef(vcls);
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  }
}

jlong JNICALL PresentDriver_nOpen(JNIEnv* env, jclass, jobject view, jint w, jint h,
                                  jstring title, jstring appId) {
  if (!wm::wayland_available()) {
    log::i(kTag, "PresentDriver.nOpen: no Wayland compositor reachable");
    return 0;
  }
  auto begin = reinterpret_cast<long (*)(int, int)>(
      ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
  auto finish = reinterpret_cast<bool (*)(int, PresentDmabuf*)>(
      ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
  if (begin == nullptr || finish == nullptr) {
    log::w(kTag, "PresentDriver.nOpen: GPU begin/end frame missing (libacl_graphics_jni?)");
    return 0;
  }
  auto* s = new PresentSession();
  env->GetJavaVM(&s->vm);
  s->w = w;
  s->h = h;
  const jint ms_exactly = 1 << 30;  // MeasureSpec.EXACTLY
  const jint ms_mask = 0x3 << 30;
  s->wspec = (w & ~ms_mask) | ms_exactly;
  s->hspec = (h & ~ms_mask) | ms_exactly;
  s->gpu_begin = begin;
  s->gpu_end = finish;
  s->deliver_vsync = reinterpret_cast<void (*)(JNIEnv*, jlong)>(::dlsym(RTLD_DEFAULT, "acl_deliver_vsync"));
  s->vsync_pending = reinterpret_cast<int (*)()>(::dlsym(RTLD_DEFAULT, "acl_vsync_pending"));
  s->vsync_set_immediate = reinterpret_cast<void (*)(int)>(::dlsym(RTLD_DEFAULT, "acl_vsync_set_immediate"));
  s->view = env->NewGlobalRef(view);

  // Resolve the View traversal methods + input dispatch entry points.
  if (jclass vc = env->GetObjectClass(view)) {
    s->m_measure = env->GetMethodID(vc, "measure", "(II)V");
    s->m_gmw = env->GetMethodID(vc, "getMeasuredWidth", "()I");
    s->m_gmh = env->GetMethodID(vc, "getMeasuredHeight", "()I");
    s->m_layout = env->GetMethodID(vc, "layout", "(IIII)V");
    s->m_draw = env->GetMethodID(vc, "draw", "(Landroid/graphics/Canvas;)V");
    s->dispatch_touch = env->GetMethodID(vc, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
    s->dispatch_key = env->GetMethodID(vc, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
    env->DeleteLocalRef(vc);
  }
  // Overlay input routing (Dialog/PopupWindow gets input first). Best-effort — absent it, input
  // falls through to the Activity as before.
  if (jclass hb = env->FindClass("org/acl/internal/HostBootstrap")) {
    s->hostbootstrap_cls = static_cast<jclass>(env->NewGlobalRef(hb));
    s->route_overlay_touch = env->GetStaticMethodID(hb, "routeOverlayTouch", "(Landroid/view/MotionEvent;)Z");
    env->DeleteLocalRef(hb);
  }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  if (jclass mecls = env->FindClass("android/view/MotionEvent")) {
    s->me_obtain = env->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
    s->me_recycle = env->GetMethodID(mecls, "recycle", "()V");
    s->motionevent_cls = static_cast<jclass>(env->NewGlobalRef(mecls));
    env->DeleteLocalRef(mecls);
  } else { env->ExceptionClear(); }
  if (jclass kecls = env->FindClass("android/view/KeyEvent")) {
    s->ke_obtain = env->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
    s->keyevent_cls = static_cast<jclass>(env->NewGlobalRef(kecls));
    env->DeleteLocalRef(kecls);
  } else { env->ExceptionClear(); }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();

  auto to_utf8 = [&](jstring js) -> std::string {
    if (js == nullptr) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string out(c != nullptr ? c : "");
    if (c != nullptr) env->ReleaseStringUTFChars(js, c);
    return out;
  };
  const std::string t = to_utf8(title);
  std::string a = to_utf8(appId);
  // ACL_APP_ID overrides the xdg_toplevel app_id — diagnostic for whether gnome-shell gates its OSK
  // on the focused window being a tracked application (a real .desktop app_id) vs acl's package.
  if (const char* e = std::getenv("ACL_APP_ID"); e != nullptr && *e != '\0') a = e;

  const wm::InputCallback input = [s](const wm::InputEvent& ev) { present_deliver_input(s, ev); };
  s->presenter = wm::presenter_open(w, h, t.empty() ? "acl" : t, a.empty() ? "org.acl.app" : a,
                                    input, /*want_ime=*/true);
  if (!s->presenter) {
    log::w(kTag, "PresentDriver.nOpen: presenter_open failed");
    if (s->view != nullptr) env->DeleteGlobalRef(s->view);
    if (s->motionevent_cls != nullptr) env->DeleteGlobalRef(s->motionevent_cls);
    if (s->keyevent_cls != nullptr) env->DeleteGlobalRef(s->keyevent_cls);
    delete s;
    return 0;
  }
  // Deferred present (UI-thread/RenderThread split): when ACL_PRESENT_RT is set, the present-root
  // records into an SkPicture replayed on a dedicated RenderThread that owns all GrContext work, so
  // the UI thread does none (GrDirectContext is single-threaded). Otherwise, the immediate path wraps
  // the persistent GPU SkCanvas here (begin_frame clears it each tick, same handle) and the
  // present-root RecordingCanvas draws straight onto it.
  s->deferred = std::getenv("ACL_PRESENT_RT") != nullptr;
  const jlong ch = s->deferred ? 1 : s->gpu_begin(w, h);  // deferred: device inits on the RT's 1st frame
  if (ch != 0) {
    if (s->deferred) {
      // Enable deferral BEFORE the present-root beginRecording below so its RecordingCanvas records.
      if (auto set_defer =
              reinterpret_cast<void (*)(int)>(::dlsym(RTLD_DEFAULT, "acl_graphics_set_present_defer")))
        set_defer(1);
      s->rthread = std::make_unique<RenderThread>();
      s->rthread->start(s);
      log::i(kTag, "PresentDriver.nOpen: deferred present + RenderThread enabled (ACL_PRESENT_RT)");
    } else if (auto set_present = reinterpret_cast<void (*)(jlong)>(
                   ::dlsym(RTLD_DEFAULT, "acl_graphics_set_present_canvas"))) {
      // Compose paints via Canvas.drawRenderNode, which requires an isHardwareAccelerated()
      // RecordingCanvas — a plain Canvas throws. So back the frame with a "present-root" RenderNode:
      // graphics_jni maps that node's RecordingCanvas onto the GPU SkCanvas (ch), so its draws land
      // immediately AND Compose's RenderNode layers replay through it.
      set_present(ch);
    }
    // Register acl's bionic-%fs RunScope wrappers for the WebView AwDrawFn Vulkan functor into
    // graphics_jni (webview_functor.cpp), so its SkDrawable can drive Chromium's init_vk/draw_vk/
    // post_draw_vk. Once per process; graphics_jni is loaded by here.
    if (static bool wv_hooked = false; !wv_hooked) {
      if (auto set_hooks =
              reinterpret_cast<void (*)(void (*)(int, void*), void (*)(int, void*),
                                        void (*)(int, void*), void (*)(int, void*))>(
                  ::dlsym(RTLD_DEFAULT, "acl_graphics_set_webview_vk_hooks"))) {
        set_hooks(&acl_webview_functor_vk_sync, &acl_webview_functor_vk_init,
                  &acl_webview_functor_vk_draw, &acl_webview_functor_vk_post_draw);
        wv_hooked = true;
      }
    }
    bool recording = false;
    if (jclass rnC = env->FindClass("android/graphics/RenderNode")) {
      jmethodID rnCtor = env->GetMethodID(rnC, "<init>", "(Ljava/lang/String;)V");
      jmethodID beginRec =
          env->GetMethodID(rnC, "beginRecording", "(II)Landroid/graphics/RecordingCanvas;");
      if (rnCtor != nullptr && beginRec != nullptr) {
        jstring nm = env->NewStringUTF("acl$present$root");
        jobject node = env->NewObject(rnC, rnCtor, nm);
        env->DeleteLocalRef(nm);
        if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }
        if (node != nullptr && env->ExceptionCheck() == JNI_FALSE) {
          jobject c = env->CallObjectMethod(node, beginRec, w, h);
          if (c != nullptr && env->ExceptionCheck() == JNI_FALSE) {
            s->canvas = env->NewGlobalRef(c);
            s->root_node = env->NewGlobalRef(node);
            recording = true;
            env->DeleteLocalRef(c);
          }
          env->DeleteLocalRef(node);
        }
      }
      env->DeleteLocalRef(rnC);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    // Fallback: no RecordingCanvas (older/odd framework) — a plain Canvas still renders classic Views.
    if (!recording) {
      if (jclass canvasC = env->FindClass("android/graphics/Canvas")) {
        if (jmethodID cctor = env->GetMethodID(canvasC, "<init>", "(J)V")) {
          jobject c = env->NewObject(canvasC, cctor, ch);
          if (c != nullptr) { s->canvas = env->NewGlobalRef(c); env->DeleteLocalRef(c); }
        }
        env->DeleteLocalRef(canvasC);
      }
      log::w(kTag, "PresentDriver.nOpen: RecordingCanvas unavailable; Compose layers will not replay");
    }
  }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  log::i(kTag, "PresentDriver.nOpen: {}x{} presenter ready (looper-driven)", w, h);
  return reinterpret_cast<jlong>(s);
}

jint JNICALL PresentDriver_nFd(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  return (s != nullptr && s->presenter) ? s->presenter->fd() : -1;
}
// IME show/hide from HostBootstrap's InputMethodManager interception (a focused text field). Runs
// on the looper/present thread (approach A), so it can drive the presenter's text-input directly.
void JNICALL PresentDriver_nSetImeVisible(JNIEnv*, jclass, jlong sp, jboolean visible) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s != nullptr && s->presenter) s->presenter->set_ime_visible(visible == JNI_TRUE);
}
jint JNICALL PresentDriver_nRepeatFd(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  return (s != nullptr && s->presenter) ? s->presenter->repeat_fd() : -1;
}
jboolean JNICALL PresentDriver_nDispatch(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr || !s->presenter) return JNI_FALSE;
  const bool ok = s->presenter->dispatch();  // runs input callbacks; wp_presentation OnPresented lands here
  // Sample the latest real presentation feedback (present thread == OnPresented thread) for the
  // RenderThread to forward to Chromium's WebView compositor next frame.
  std::uint64_t pn = 0;
  std::uint32_t rn = 0, fl = 0;
  if (s->presenter->last_present_feedback(&pn, &rn, &fl)) {
    s->pf_present_ns.store(pn, std::memory_order_relaxed);
    s->pf_refresh_ns.store(rn, std::memory_order_relaxed);
    s->pf_flags.store(fl, std::memory_order_relaxed);
  }
  return ok ? JNI_TRUE : JNI_FALSE;
}
void JNICALL PresentDriver_nDispatchRepeat(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s != nullptr && s->presenter) s->presenter->dispatch_repeat();
}
void JNICALL PresentDriver_nSetVsyncDelivered(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s != nullptr && s->vsync_set_immediate != nullptr) s->vsync_set_immediate(0);
}
void JNICALL PresentDriver_nMaybeDeliverVsync(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr || !s->presenter || s->deliver_vsync == nullptr) return;
  const bool rdy = s->presenter->ready();
  const bool pend = (s->vsync_pending == nullptr || s->vsync_pending() != 0);
  static int diag = 0;
  if (diag < 8) { log::i(kTag, "PresentDriver.vsync: ready={} pending={}", rdy, pend); ++diag; }
  if (rdy && pend) s->deliver_vsync(env, static_cast<jlong>(s->presenter->frame_time_ns()));
}
// Build a gfx::DmabufFrame from a produced PresentDmabuf and present it; logs + bumps the presented-
// frame count. Shared by the immediate path and the deferred RenderThread path. Returns false if the
// dma-buf is invalid or submit failed (caller keepalives).
static bool present_submit(PresentSession* s, const PresentDmabuf& f, int slot) {
  if (f.fd < 0) return false;
  gfx::DmabufFrame fr{};
  fr.fd = f.fd;
  fr.width = f.width;
  fr.height = f.height;
  fr.plane_count = f.plane_count;
  for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
    fr.planes[i].offset = f.offsets[i];
    fr.planes[i].stride = f.strides[i];
  }
  fr.modifier = f.modifier;
  fr.drm_format = f.drm_format;
  fr.modifier_tiled = f.modifier != 0;
  if (!s->presenter->submit(fr, slot)) return false;
  // ACL_PRESENT_DEFER_DEBUG=1: log EVERY submit with its buffer slot. The periodic log below only
  // fires on frame 0 and every 60th, which reads as "only frame 0 was presented" for any app that
  // presents slowly -- exactly the apps being diagnosed.
  static const bool submit_debug = [] {
    const char* v = std::getenv("ACL_PRESENT_DEFER_DEBUG");
    return v != nullptr && *v != '\0' && *v != '0';
  }();
  if (submit_debug) log::i(kTag, "present_submit: frame {} slot {} fd {}", s->frames, slot, f.fd);
  if (s->frames == 0 || s->frames % 60 == 0)
    log::i(kTag, "PresentDriver: frame {} presented ({}x{}) — looper-driven", s->frames, f.width,
           f.height);
  ++s->frames;
  return true;
}
void JNICALL PresentDriver_nTraverse(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr || !s->presenter) return;
  static int tdiag = 0;
  if (tdiag < 4) { log::i(kTag, "PresentDriver.nTraverse: entered (frame {})", s->frames); ++tdiag; }
  // DIAG: env-gated synthetic tap (ACL_TEST_TAP="x,y[,frame]") — inject Down then Up at the point
  // on frame N (default 120) to exercise the input -> hit-test -> recompose chain without a live
  // pointer. Down and Up two frames apart so Compose registers a full press+release.
  static float tap_x = -1.f, tap_y = -1.f;
  static long tap_frame = 120;
  static int tap_state = 0;
  static bool tap_parsed = false;
  if (!tap_parsed) {
    tap_parsed = true;
    if (const char* t = std::getenv("ACL_TEST_TAP")) {
      float x = 0, y = 0;
      long fr = 120;
      if (std::sscanf(t, "%f,%f,%ld", &x, &y, &fr) >= 2) { tap_x = x; tap_y = y; tap_frame = fr; }
    }
  }
  if (tap_x >= 0.f) {
    if (s->frames == static_cast<std::uint64_t>(tap_frame) && tap_state == 0) {
      log::i(kTag, "input: synthetic TAP down at ({},{})", tap_x, tap_y);
      present_deliver_input(s, wm::PointerInput{wm::PointerInput::Action::Down, tap_x, tap_y});
      tap_state = 1;
    } else if (s->frames == static_cast<std::uint64_t>(tap_frame) + 2 && tap_state == 1
               && std::getenv("ACL_TEST_HOLD") == nullptr) {
      // ACL_TEST_HOLD keeps the pointer down (no Up) so the pressed state / ripple persists for a
      // screen capture; otherwise release to complete the tap (fires the composable's onClick).
      present_deliver_input(s, wm::PointerInput{wm::PointerInput::Action::Up, tap_x, tap_y});
      log::i(kTag, "input: synthetic TAP up");
      tap_state = 2;
    }
  }
  // DIAG: env-gated synthetic drag (ACL_TEST_DRAG="x1,y1,x2,y2[,frame]") — Down at (x1,y1) once the
  // loop reaches frame N, interpolated Moves to (x2,y2) over the next frames, then Up. Touch-drawing
  // apps (S.N.A.K.E's fading-stroke canvas) only produce content from a drag gesture, not a tap.
  static float drg[4] = {-1.f, 0.f, 0.f, 0.f};
  static long drag_frame = 120;
  static int drag_step = -1;  // -1 = not started; 0..kSteps = in progress
  static bool drag_parsed = false;
  constexpr int kDragSteps = 12;
  if (!drag_parsed) {
    drag_parsed = true;
    if (const char* d = std::getenv("ACL_TEST_DRAG")) {
      float a = 0, b = 0, c = 0, e = 0;
      long fr = 120;
      if (std::sscanf(d, "%f,%f,%f,%f,%ld", &a, &b, &c, &e, &fr) >= 4) {
        drg[0] = a; drg[1] = b; drg[2] = c; drg[3] = e; drag_frame = fr;
      }
    }
  }
  if (drg[0] >= 0.f) {
    if (drag_step < 0 && s->frames >= static_cast<std::uint64_t>(drag_frame)) {
      present_deliver_input(s, wm::PointerInput{wm::PointerInput::Action::Down, drg[0], drg[1]});
      log::i(kTag, "input: synthetic DRAG down at ({},{})", drg[0], drg[1]);
      drag_step = 0;
    } else if (drag_step >= 0 && drag_step < kDragSteps) {
      ++drag_step;
      const float t = static_cast<float>(drag_step) / static_cast<float>(kDragSteps);
      const float x = drg[0] + (drg[2] - drg[0]) * t;
      const float y = drg[1] + (drg[3] - drg[1]) * t;
      present_deliver_input(s, wm::PointerInput{wm::PointerInput::Action::Move, x, y});
      if (drag_step == kDragSteps) {
        present_deliver_input(s, wm::PointerInput{wm::PointerInput::Action::Up, drg[2], drg[3]});
        log::i(kTag, "input: synthetic DRAG up at ({},{})", drg[2], drg[3]);
      }
    }
  }
  // Deferred one-frame-behind pipeline: if the RenderThread is still rendering the previous frame,
  // skip this tick WITHOUT blocking — the Looper keeps ticking so the Choreographer drives the
  // WebView renderer's begin-frames while the RT waits on it (the crux of the fix). When the RT is
  // idle, present its just-completed frame, then record + post the next one below.
  if (s->deferred) {
    if (s->rthread->busy()) {
      s->presenter->keepalive();
      return;
    }
    PresentDmabuf done{};
    int done_slot = -1;
    if (s->rthread->take_completed(&done, &done_slot) && !present_submit(s, done, done_slot))
      s->presenter->keepalive();
  }
  // Immediate mode clears + re-arms the GPU canvas on this thread; deferred mode does gpu_begin on the
  // RenderThread (below), so the UI thread touches no GrContext.
  if (!s->deferred && s->gpu_begin(s->w, s->h) == 0) {  // clear + re-arm
    log::w(kTag, "PresentDriver.nTraverse: gpu_begin=0");
    s->presenter->keepalive();  // no submit this frame — keep the vsync loop ticking
    return;
  }
  const bool diag = tdiag <= 4;
  if (s->m_measure != nullptr && s->m_layout != nullptr) {
    env->CallVoidMethod(s->view, s->m_measure, s->wspec, s->hspec);
    const jint mw = env->CallIntMethod(s->view, s->m_gmw);
    const jint mh = env->CallIntMethod(s->view, s->m_gmh);
    env->CallVoidMethod(s->view, s->m_layout, 0, 0, mw, mh);
    if (diag) log::i(kTag, "PresentDriver.nTraverse: measured {}x{}", mw, mh);
    // DIAG (ACL_VIEWTREE_DIAG): decor-tree dump after layout, to pin a blank frame or an
    // input-routing gap to view bounds vs. tree structure. The value is the traversal interval:
    // unset/1 dumps once after the first layout, N>1 dumps every N traversals (up to 6 dumps).
    // A single early dump is misleading for apps that populate the tree asynchronously -- React
    // Native mounts its views well after the first layout, so the one-shot dump shows an empty
    // ReactSurfaceView no matter what the app eventually renders.
    // Counted in TRAVERSALS, not frames: an app that never completes a frame (the case this
    // exists to diagnose) leaves s->frames pinned while traversals keep running.
    static const int vtree_every = [] {
      const char* v = std::getenv("ACL_VIEWTREE_DIAG");
      if (v == nullptr || *v == '\0') return 0;
      const int n = std::atoi(v);
      return n > 1 ? n : 1;
    }();
    static long vtree_seen = 0;
    static int vtree_dumps = 0;
    if (vtree_every > 0 && vtree_dumps < 6 && (++vtree_seen % vtree_every) == 0) {
      ++vtree_dumps;
      log::i(kTag, "PresentDriver.nTraverse: view tree after {} traversals (frame {})", vtree_seen,
             s->frames);
      if (jclass hb = env->FindClass("org/acl/internal/HostBootstrap")) {
        if (jmethodID d = env->GetStaticMethodID(hb, "dumpViewTree", "(Landroid/view/View;I)V"))
          env->CallStaticVoidMethod(hb, d, s->view, 0);
        if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
        env->DeleteLocalRef(hb);
      }
    }
  }
  if (env->ExceptionCheck() == JNI_TRUE) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    s->presenter->keepalive();  // measure/layout threw — keep ticking, retry next frame
    return;
  }
  // DIAG (Compose draw-path): count draw ops issued during this frame's draw. ~0 => the view
  // tree produced no paint (blank/white frame); high => content draws (issue is elsewhere).
  static auto drawops = reinterpret_cast<std::uint64_t (*)()>(::dlsym(RTLD_DEFAULT, "acl_graphics_profile_drawops"));
  const std::uint64_t ops_before = drawops ? drawops() : 0;
  // Force Compose layers to re-record each frame (main content AND overlays). The draw_rendernode
  // drawable fix defers CHILD replay correctly, but a parent layer that Compose caches (never
  // re-records) keeps a STALE display list across a navigation/content change — its drawables still
  // reference the OLD children, so a navigated-to or settled screen (forkyz's Settings/Help, a
  // Dialog/Popup) paints blank. Re-recording the whole tree every frame keeps it fresh. acl already
  // re-draws every frame, so this trades CPU for correct rendering; the present-root is exempt.
  //
  // ACL_FORCE_RECORD=0 turns it off. It is not free: it invalidates EVERY RenderNode every frame
  // (RenderNode_nIsValid returns false), so View.updateDisplayListIfDirty re-records the whole
  // tree. A Compose app's layer tree is shallow, but a React Native tree is one ReactViewGroup per
  // JS view -- castlab draws ~110 levels deep, each with a BorderDrawable background, and
  // re-recording all of it every frame in the ART interpreter is what pins that app at ~1.7s per
  // traversal (a white window that never settles).
  static auto force_record = reinterpret_cast<void (*)(int)>(
      ::dlsym(RTLD_DEFAULT, "acl_graphics_set_force_record"));
  static const bool force_record_on = [] {
    const char* v = std::getenv("ACL_FORCE_RECORD");
    return v == nullptr || *v == '\0' || v[0] != '0';
  }();
  if (force_record != nullptr) force_record(force_record_on ? 1 : 0);
  const auto draw_t0 = std::chrono::steady_clock::now();
  // Deferred present (ACL_PRESENT_RT): (re)begin the present-root picture recording for this frame,
  // just before View.draw records into s->canvas. No-op in the default immediate path.
  static auto begin_record = reinterpret_cast<void (*)(int, int)>(
      ::dlsym(RTLD_DEFAULT, "acl_graphics_present_begin_record"));
  if (begin_record != nullptr) begin_record(s->w, s->h);
  if (s->m_draw != nullptr && s->canvas != nullptr) env->CallVoidMethod(s->view, s->m_draw, s->canvas);
  if (env->ExceptionCheck() == JNI_TRUE) {
    env->ExceptionDescribe(); env->ExceptionClear();
    if (force_record != nullptr) force_record(0);
    s->presenter->keepalive();  // View.draw threw — keep ticking, retry next frame
    return;
  }
  // Overlay pass: composite any Dialog/PopupWindow decors (WindowManagerGlobal.mViews) over the
  // Activity in the same surface. HostBootstrap owns the enumeration/measure/layout/draw.
  static jclass overlay_cls = nullptr;
  static jmethodID overlay_present = nullptr;
  if (overlay_present == nullptr) {
    if (jclass b = env->FindClass("org/acl/internal/HostBootstrap")) {
      overlay_cls = static_cast<jclass>(env->NewGlobalRef(b));
      overlay_present = env->GetStaticMethodID(b, "presentOverlays", "(Landroid/graphics/Canvas;II)V");
      env->DeleteLocalRef(b);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  }
  // DIAG (ACL_OVERLAY_DIAG): the draw ops + RenderNode replays a Dialog/PopupWindow overlay pass
  // contributes this frame. Compose content rides in replays (not direct ops), so both are logged;
  // 0/0 means the decor painted nothing (empty composition). Confirms overlay compositing lands.
  static const bool ov_diag = std::getenv("ACL_OVERLAY_DIAG") != nullptr;
  static auto recstats0 = reinterpret_cast<std::uint64_t (*)()>(::dlsym(RTLD_DEFAULT, "acl_graphics_profile_recstats"));
  const std::uint64_t ops_pre_overlay = ov_diag && drawops ? drawops() : 0;
  const std::uint64_t rep_pre_overlay = ov_diag && recstats0 ? (recstats0() & 0xfffff) : 0;
  if (overlay_present != nullptr && s->canvas != nullptr)
    env->CallStaticVoidMethod(overlay_cls, overlay_present, s->canvas, s->w, s->h);
  if (force_record != nullptr) force_record(0);  // whole frame drawn; re-arm caching for the next
  // Deferred present (ACL_PRESENT_RT): finalize the present-root picture (View.draw + overlays) so
  // gpu_end can replay it onto the GPU canvas. No-op in the default immediate path.
  static auto finish_record =
      reinterpret_cast<void (*)()>(::dlsym(RTLD_DEFAULT, "acl_graphics_present_finish_record"));
  if (finish_record != nullptr) finish_record();
  if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }
  if (ov_diag) {
    const std::uint64_t d = drawops ? drawops() - ops_pre_overlay : 0;
    const std::uint64_t rep_d = recstats0 ? (recstats0() & 0xfffff) - rep_pre_overlay : 0;
    static int shown = 0;
    if ((d > 0 || rep_d > 0) && (shown < 8 || s->frames % 60 == 0)) {
      log::i(kTag, "overlay pass: {} draw ops, {} replays (frame {})", d, rep_d, s->frames);
      ++shown;
    }
  }
  static auto rnodes = reinterpret_cast<std::uint64_t (*)()>(::dlsym(RTLD_DEFAULT, "acl_graphics_profile_rendernodes"));
  static auto recstats = reinterpret_cast<std::uint64_t (*)()>(::dlsym(RTLD_DEFAULT, "acl_graphics_profile_recstats"));
  if (diag) {
    const std::uint64_t rs = recstats ? recstats() : 0;
    // Draw wall time separates "the tree is expensive to record" from "the loop is not ticking":
    // both look like a frozen window, and only the first is acl's re-record cost.
    const auto draw_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - draw_t0)
                             .count();
    log::i(kTag, "PresentDriver.nTraverse: drawn in {}ms ({} draw ops this frame, {} RenderNodes "
                 "created; rec begins={} finishes={} replays={})",
           draw_ms, drawops ? drawops() - ops_before : 0, rnodes ? rnodes() : 0,
           (rs >> 40) & 0xfffff, (rs >> 20) & 0xfffff, rs & 0xfffff);
  }
  const int bufidx = s->presenter->next_buffer();
  if (bufidx < 0) {
    if (diag) log::w(kTag, "PresentDriver.nTraverse: no free buffer");
    s->presenter->keepalive();  // no free ping-pong buffer — keep ticking until one releases
    return;
  }
  if (s->deferred) {
    // Snapshot the just-recorded picture and hand it to the RenderThread; it renders into `bufidx`
    // (gpu_begin clear + replay + flush/copy, incl. the WebView functor) while this UI thread returns
    // and keeps the Looper/Choreographer running. The result is presented next idle tick above.
    static auto take_for_render =
        reinterpret_cast<void (*)()>(::dlsym(RTLD_DEFAULT, "acl_graphics_present_take_for_render"));
    if (take_for_render != nullptr) take_for_render();
    if (!s->rt_bootstrapped) {
      // First frame: render synchronously and submit inline. keepalive() is a no-op until a frame has
      // been shown (last_slot_<0), so the async path can't arm the vsync loop from cold — bootstrap it.
      PresentDmabuf f0{};
      if (s->rthread->render_sync(s->w, s->h, bufidx, &f0) && present_submit(s, f0, bufidx))
        s->rt_bootstrapped = true;
      else
        s->presenter->keepalive();
      return;
    }
    s->rthread->post(s->w, s->h, bufidx);
    s->presenter->keepalive();  // the RT presents async — keep the vsync loop ticking so a later
    return;                     // idle tick picks up the completed frame and submits it
  }
  PresentDmabuf f{};
  if (!s->gpu_end(bufidx, &f)) {  // immediate: replay-less flush/copy inline on this thread
    if (diag) log::w(kTag, "PresentDriver.nTraverse: gpu_end failed");
    s->presenter->keepalive();  // GPU end-frame failed — keep ticking, retry next frame
    return;
  }
  if (!present_submit(s, f, bufidx))
    s->presenter->keepalive();  // submit failed (buffer build) — keep the vsync loop ticking
}
void JNICALL PresentDriver_nClose(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr) return;
  if (s->rthread) { s->rthread->stop(); s->rthread.reset(); }  // join the GPU thread before teardown
  s->presenter.reset();  // tears down the surface + disconnects
  if (s->view != nullptr) env->DeleteGlobalRef(s->view);
  if (s->canvas != nullptr) env->DeleteGlobalRef(s->canvas);
  if (s->root_node != nullptr) env->DeleteGlobalRef(s->root_node);
  if (s->motionevent_cls != nullptr) env->DeleteGlobalRef(s->motionevent_cls);
  if (s->keyevent_cls != nullptr) env->DeleteGlobalRef(s->keyevent_cls);
  delete s;
}

// Bind org.acl.internal.PresentDriver's native seam. RegisterNatives is atomic per class, so
// every signature must match; a miss leaves PresentDriver.run to fail fast via nOpen=0.
#if ACL_HAVE_GL
// Create the view-independent GL present session: the toplevel presenter, the wayland GlDevice on
// it, and the vsync timerfd; publish g_gl_session (display + wl_surface) so the app's native EGL
// (get_display / eglCreateWindowSurface) resolves. NO decor View is attached yet — input is dropped
// (present_deliver_input null-checks the class refs) until nGlOpen wires it. Returns nullptr on
// failure. MUST run on the app main thread (the get_display hook during the engine's onCreate GL
// init, or nGlOpen): the wl_display connection then lives on the thread runGl later dispatches it on.
PresentSession* create_gl_session_core(JavaVM* vm, int w, int h,
                                       const std::string& title, const std::string& appId) {
  if (!wm::wayland_available()) { log::i(kTag, "GL session: no Wayland compositor reachable"); return nullptr; }
  auto* s = new PresentSession();
  s->is_gl = true;
  s->vm = vm;
  s->w = w;
  s->h = h;
  s->deliver_vsync =
      reinterpret_cast<void (*)(JNIEnv*, jlong)>(::dlsym(RTLD_DEFAULT, "acl_deliver_vsync"));
  const wm::InputCallback input = [s](const wm::InputEvent& ev) { present_deliver_input(s, ev); };
  s->presenter = wm::presenter_open(w, h, title.empty() ? "acl" : title,
                                    appId.empty() ? "org.acl.app" : appId, input, /*want_ime=*/false);
  if (!s->presenter) { log::w(kTag, "GL session: presenter_open failed"); delete s; return nullptr; }
  s->presenter->wait_configured(2000);  // GL device is built on the configured toplevel surface
  std::string err;
  auto dev = gfx::GlDevice::CreateWayland(s->presenter->gl_display(), &err);
  if (!dev) {
    log::w(kTag, "GL session: CreateWayland failed: {}", err);
    s->presenter.reset();
    delete s;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(g_gl_session.mtx);
    g_gl_session.device = std::move(dev);
    g_gl_session.wl_surface = s->presenter->gl_surface();
    g_gl_session.w = w;
    g_gl_session.h = h;
    g_gl_session.ready = true;
  }
  g_gl_session.cv.notify_all();
  s->vsync_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (s->vsync_fd >= 0) {
    itimerspec its{};
    its.it_value.tv_nsec = 16'666'666;
    its.it_interval.tv_nsec = 16'666'666;  // 60 Hz
    ::timerfd_settime(s->vsync_fd, 0, &its, nullptr);
  }
  log::i(kTag, "GL session: device on acl's toplevel wl_surface ({}x{}, {})", w, h,
         g_gl_session.device->renderer());
  return s;
}

// Attach the decor View + input class refs to a GL session (fresh or adopted), inject the Surface
// into the SurfaceView so the engine binds its onscreen EGL, and lay out the decor for hit-testing.
// The presenter/device/vsync-fd already exist (create_gl_session_core); this is the View-dependent
// half of nGlOpen, run once the decor exists (post-onCreate).
void attach_view_to_gl_session(JNIEnv* env, PresentSession* s, jobject view, int w, int h) {
  s->view = env->NewGlobalRef(view);
  if (jclass vc = env->GetObjectClass(view)) {
    s->m_measure = env->GetMethodID(vc, "measure", "(II)V");
    s->m_layout = env->GetMethodID(vc, "layout", "(IIII)V");
    s->dispatch_touch = env->GetMethodID(vc, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
    s->dispatch_key = env->GetMethodID(vc, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
    env->DeleteLocalRef(vc);
  }
  if (jclass hb = env->FindClass("org/acl/internal/HostBootstrap")) {
    s->hostbootstrap_cls = static_cast<jclass>(env->NewGlobalRef(hb));
    s->route_overlay_touch =
        env->GetStaticMethodID(hb, "routeOverlayTouch", "(Landroid/view/MotionEvent;)Z");
    env->DeleteLocalRef(hb);
  }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  if (jclass mecls = env->FindClass("android/view/MotionEvent")) {
    s->me_obtain = env->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
    s->me_recycle = env->GetMethodID(mecls, "recycle", "()V");
    s->motionevent_cls = static_cast<jclass>(env->NewGlobalRef(mecls));
    env->DeleteLocalRef(mecls);
  } else {
    env->ExceptionClear();
  }
  if (jclass kecls = env->FindClass("android/view/KeyEvent")) {
    s->ke_obtain = env->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
    s->keyevent_cls = static_cast<jclass>(env->NewGlobalRef(kecls));
    env->DeleteLocalRef(kecls);
  } else {
    env->ExceptionClear();
  }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();

  // Inject the Surface into the SurfaceView (HostBootstrap.injectGlSurface) so its holder's
  // surfaceCreated fires and Flutter's engine binds its native EGL to it; lay out the decor so
  // pointer events hit-test down to the SurfaceView.
  if (jclass hb = env->FindClass("org/acl/internal/HostBootstrap")) {
    if (jmethodID m = env->GetStaticMethodID(hb, "injectGlSurface", "(Landroid/view/View;II)V"))
      env->CallStaticVoidMethod(hb, m, view, w, h);
    env->DeleteLocalRef(hb);
  }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  if (s->m_measure != nullptr && s->m_layout != nullptr) {
    const jint mask = 0x3 << 30;  // MeasureSpec.EXACTLY(size)
    env->CallVoidMethod(view, s->m_measure, (w & ~mask) | (1 << 30), (h & ~mask) | (1 << 30));
    env->CallVoidMethod(view, s->m_layout, 0, 0, w, h);
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  }
  // Switch vsync from self-clocked (immediate) to DELIVERED: scheduleVsync only marks a request
  // pending, and our timerfd tick delivers it at the wp_presentation-aligned frame_time_ns.
  if (auto set_imm = reinterpret_cast<void (*)(int)>(::dlsym(RTLD_DEFAULT, "acl_vsync_set_immediate")))
    set_imm(0);
  log::i(kTag, "nGlOpen: {}x{} GL present ready (looper-driven, wp_presentation vsync)", w, h);
}
#endif

// ── GL present (Flutter / SurfaceView): Looper-driven, but Mesa renders + commits the toplevel ──
// Same Looper model as nOpen (reuses nFd/nDispatch/nRepeatFd/nDispatchRepeat) but with NO Skia
// presenter and NO nTraverse: acl builds the GL device on the toplevel wl_surface, injects the
// SurfaceView's Surface (Flutter's raster thread renders + eglSwapBuffers there), and clocks the
// real Choreographer's vsync from a timerfd — each tick armed with wp_presentation feedback so
// frame_time_ns() tracks Mesa's commits (the wp_presentation clock, delivered via acl_deliver_vsync).
// If the engine's on-main-thread GL init already created the session (get_display hook), ADOPT it
// here instead of opening a second presenter.
jlong JNICALL PresentDriver_nGlOpen(JNIEnv* env, jclass, jobject view, jint w, jint h,
                                    jstring title, jstring appId) {
#if ACL_HAVE_GL
  log::i(kTag, "nGlOpen: entry ({}x{}) — backing a SurfaceView with an acl GL session", w, h);
  if (!wm::wayland_available()) { log::i(kTag, "nGlOpen: no Wayland compositor reachable"); return 0; }
  // Adopt a session the get_display hook pre-created during the engine's onCreate GL init, if any.
  PresentSession* pending = nullptr;
  { std::lock_guard<std::mutex> lk(g_pending_gl_mtx); pending = g_pending_gl_session; g_pending_gl_session = nullptr; }
  if (pending != nullptr) {
    log::i(kTag, "nGlOpen: adopting pre-created GL session (built during the engine's GL init)");
    PresentSession* s = pending;
    attach_view_to_gl_session(env, s, view, w, h);
    return reinterpret_cast<jlong>(s);
  }
  // No pre-created session (e.g. an app whose GL init did not call get_display before nGlOpen, or a
  // GLSurfaceView app): build it now, then attach the decor.
  auto to_utf8 = [&](jstring js) -> std::string {
    if (js == nullptr) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string out(c != nullptr ? c : "");
    if (c != nullptr) env->ReleaseStringUTFChars(js, c);
    return out;
  };
  JavaVM* vm = nullptr;
  env->GetJavaVM(&vm);
  PresentSession* s = create_gl_session_core(vm, w, h, to_utf8(title), to_utf8(appId));
  if (s == nullptr) { log::w(kTag, "nGlOpen: GL session create failed"); return 0; }
  attach_view_to_gl_session(env, s, view, w, h);
  return reinterpret_cast<jlong>(s);
#else
  (void)env; (void)view; (void)w; (void)h; (void)title; (void)appId;
  return 0;
#endif
}

jint JNICALL PresentDriver_nGlVsyncFd(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  return s != nullptr ? s->vsync_fd : -1;
}
// Drain the timerfd, arm wp_presentation feedback for Mesa's next commit, and tick the Choreographer.
void JNICALL PresentDriver_nGlVsync(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr || !s->presenter) return;
  if (s->vsync_fd >= 0) {
    std::uint64_t exp = 0;
    while (::read(s->vsync_fd, &exp, sizeof(exp)) == static_cast<ssize_t>(sizeof(exp))) {}
  }
  s->presenter->arm_present();
  if (s->deliver_vsync != nullptr)
    s->deliver_vsync(env, static_cast<jlong>(s->presenter->frame_time_ns()));
}
void JNICALL PresentDriver_nGlClose(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr) return;
  { std::lock_guard<std::mutex> lk(g_pending_gl_mtx); if (g_pending_gl_session == s) g_pending_gl_session = nullptr; }
  {
    std::lock_guard<std::mutex> lk(g_gl_session.mtx);
    g_gl_session.window.reset();
    g_gl_session.device.reset();
    g_gl_session.wl_surface = nullptr;
    g_gl_session.ready = false;
  }
  if (s->vsync_fd >= 0) ::close(s->vsync_fd);
  s->presenter.reset();
  if (s->view != nullptr) env->DeleteGlobalRef(s->view);
  if (s->motionevent_cls != nullptr) env->DeleteGlobalRef(s->motionevent_cls);
  if (s->keyevent_cls != nullptr) env->DeleteGlobalRef(s->keyevent_cls);
  if (s->hostbootstrap_cls != nullptr) env->DeleteGlobalRef(s->hostbootstrap_cls);
  delete s;
}

// Bridge the calling thread's NDK ALooper into acl's Java Looper (acl::ndk::main_looper_bridge_*).
// Thread-scoped, no PresentSession: they act on the caller's looper, which in runGl is the app main
// thread — where Flutter's merged platform+UI task runner registers its frame timer. nGlAloopFd
// returns the ALooper's epoll fd for the Java MessageQueue to watch; nGlAloopPump drains it.
jint JNICALL PresentDriver_nGlAloopFd(JNIEnv*, jclass) {
  return static_cast<jint>(acl::ndk::main_looper_bridge_fd());
}
void JNICALL PresentDriver_nGlAloopPump(JNIEnv*, jclass) { acl::ndk::main_looper_bridge_pump(); }

void register_present_driver_natives(JNIEnv* env) {
  jclass c = env->FindClass("org/acl/internal/PresentDriver");
  if (c == nullptr) { env->ExceptionClear(); return; }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nGlOpen"),
       const_cast<char*>("(Landroid/view/View;IILjava/lang/String;Ljava/lang/String;)J"),
       reinterpret_cast<void*>(PresentDriver_nGlOpen)},
      {const_cast<char*>("nGlAloopFd"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(PresentDriver_nGlAloopFd)},
      {const_cast<char*>("nGlAloopPump"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(PresentDriver_nGlAloopPump)},
      {const_cast<char*>("nGlVsyncFd"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(PresentDriver_nGlVsyncFd)},
      {const_cast<char*>("nGlVsync"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nGlVsync)},
      {const_cast<char*>("nGlClose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nGlClose)},
      {const_cast<char*>("nOpen"),
       const_cast<char*>("(Landroid/view/View;IILjava/lang/String;Ljava/lang/String;)J"),
       reinterpret_cast<void*>(PresentDriver_nOpen)},
      {const_cast<char*>("nFd"), const_cast<char*>("(J)I"), reinterpret_cast<void*>(PresentDriver_nFd)},
      {const_cast<char*>("nSetImeVisible"), const_cast<char*>("(JZ)V"),
       reinterpret_cast<void*>(PresentDriver_nSetImeVisible)},
      {const_cast<char*>("nRepeatFd"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(PresentDriver_nRepeatFd)},
      {const_cast<char*>("nDispatch"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(PresentDriver_nDispatch)},
      {const_cast<char*>("nDispatchRepeat"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nDispatchRepeat)},
      {const_cast<char*>("nSetVsyncDelivered"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nSetVsyncDelivered)},
      {const_cast<char*>("nMaybeDeliverVsync"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nMaybeDeliverVsync)},
      {const_cast<char*>("nTraverse"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nTraverse)},
      {const_cast<char*>("nClose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nClose)},
  };
  if (env->RegisterNatives(c, m, sizeof(m) / sizeof(m[0])) != JNI_OK) {
    log::w(kTag, "PresentDriver: RegisterNatives failed");
    env->ExceptionClear();
  } else {
    log::i(kTag, "PresentDriver native seam registered (looper-driven present)");
  }
  env->DeleteLocalRef(c);
}
#endif  // ACL_HAVE_VULKAN && ACL_HAVE_WCS

#if ACL_PROFILE_AUTOMOTIVE
// The composition-root audio route for the media bridge: renders the now-playing
// track to acl::audio -> PipeWire. The demo MediaBrowserService publishes no PCM
// stream (real apps feed decoded PCM via AudioTrack, which already routes to
// acl::audio), so this plays a short representative tone — audible with
// ACL_AUDIO=pipewire, silent/deterministic with ACL_AUDIO=null.
class ToneAudioRoute : public hosts::media::AudioRoute {
 public:
  std::uint64_t play(const hosts::media::NowPlaying& now) override {
    audio::ToneSpec spec;
    spec.seconds = 0.4;
    spec.freq_hz = now.state == hosts::media::Playback::Playing ? 523 : 440;  // C5/A4
    return audio::play_tone(spec);
  }
};
#endif

std::string join(const std::vector<std::string>& parts, char sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out.push_back(sep);
    out += parts[i];
  }
  return out;
}

// Convert a dotted or slashed class name to the slashed JNI form.
std::string to_jni_name(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    if (c == '.') c = '/';
  }
  return out;
}

// Base for the two JNI-Invocation-API backends. Both embed a Java runtime via
// JNI_CreateJavaVM and drive it through identical JNI (call_static_main, the
// Activity/media/CAL lifecycles, the present path). Subclasses differ ONLY in
// create_java_vm(): JvmVm links HotSpot's JNI_CreateJavaVM (ADR-0001 fallback);
// ArtVm dlopens a host ART build's libart.so and loads DEX natively (the ADR-0001
// target). Everything below create_java_vm() is runtime-agnostic and shared.
class JniVm : public Vm {
 public:
  JniVm(Backend backend, VmConfig config)
      : Vm(backend), config_(std::move(config)) {}

  ~JniVm() override { shutdown(); }

  Error start() override {
    if (jvm_ != nullptr) return Error::ok();
    JNIEnv* env = nullptr;
    if (Error e = create_java_vm(env)) return e;
    env_ = env;
    // ArtVm ignores config_.boot_classpath (it is .class/.jar; ART needs DEX — see
    // create_java_vm), so do not print it as if ART were booting with it.
    if (backend() == Backend::Art)
      log::i(kTag, "{} started: classpath=[{}]", backend_name(backend()),
             join(config_.classpath, ':'));
    else
      log::i(kTag, "{} started: boot=[{}] classpath=[{}]", backend_name(backend()),
             join(config_.boot_classpath, ':'), join(config_.classpath, ':'));
    // Bind the android.* native methods on the classpath.
    register_natives(env_);
    return Error::ok();
  }

 protected:
  // Create and configure the underlying VM, returning its main-thread JNIEnv and
  // setting jvm_. Implemented per backend.
  virtual Error create_java_vm(JNIEnv*& env) = 0;

  // Bind the android.* native methods. Default: acl's C++ framework natives (the
  // shim floor). ArtVm can instead register the real framework natives from a host
  // libandroid_runtime (ADR-0008) — the two are alternatives, not stacked (acl's
  // shims target the shim classes and would mis-bind against the real framework).
  virtual void register_natives(JNIEnv* env) { register_framework_natives(env); }

 public:

  Error call_static_main(std::string_view main_class,
                         const std::vector<std::string>& args) override {
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_static_main before start()");
    }

    const std::string jni_name = to_jni_name(main_class);
    jclass cls = env_->FindClass(jni_name.c_str());
    if (cls == nullptr) {
      clear_exception();
      return Error::of(Status::NotFound, "class not found: " + std::string(main_class));
    }
    jmethodID mid =
        env_->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V");
    if (mid == nullptr) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      return Error::of(Status::NotFound,
                       "no static void main(String[]) in " + std::string(main_class));
    }

    jobjectArray jargs = make_string_array(args);
    if (jargs == nullptr) {
      env_->DeleteLocalRef(cls);
      return Error::of(Status::RuntimeError, "failed to build String[] args");
    }

    env_->CallStaticVoidMethod(cls, mid, jargs);
    Error result = Error::ok();
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();  // prints the stack trace to stderr
      env_->ExceptionClear();
      result = Error::of(Status::RuntimeError,
                         "uncaught exception in " + std::string(main_class) + ".main");
    }
    env_->DeleteLocalRef(jargs);
    env_->DeleteLocalRef(cls);
    return result;
  }

  Error call_activity_lifecycle(std::string_view activity_class) override {
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_activity_lifecycle before start()");
    }
    const std::string name = to_jni_name(activity_class);
    jclass cls = env_->FindClass(name.c_str());
    if (cls == nullptr) return class_load_error("class", activity_class);
    // Only drive the lifecycle for real Activity subclasses; otherwise the caller
    // falls back to a static main() (self-contained apps).
    jclass activity_cls = env_->FindClass("android/app/Activity");
    if (activity_cls == nullptr || env_->IsAssignableFrom(cls, activity_cls) != JNI_TRUE) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      if (activity_cls != nullptr) env_->DeleteLocalRef(activity_cls);
      return Error::of(Status::NotApplicable,
                       std::string(activity_class) + " is not an android.app.Activity");
    }

    // Seed android.view.Display from the compositor's wl_output before onCreate, so
    // the app's early getWindowManager()/getDefaultDisplay() metrics are real. A
    // throwaway connection; a no-op when not presenting or no compositor is reachable.
    if (config_.present) {
      int dw = 0, dh = 0, dscale = 1;
      if (wm::query_display(dw, dh, dscale)) {
        svc::window().set_display(dw, dh, dscale);
        log::i(kTag, "display from wl_output: {}x{} scale {}", dw, dh, dscale);
      }
    }

    // Run the Application BEFORE instantiating the launcher Activity (Android order): the Activity's
    // constructor / field initializers may read app state the Application sets (Telegram's
    // LaunchActivity ctor reads ApplicationLoader.applicationContext).
    ensure_application();
    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject activity = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (activity == nullptr) {
      result = fail_with_exception("instantiate " + std::string(activity_class));
    } else {
      log::i(kTag, "launching Activity {}", activity_class);
      attach_app_context(activity);  // host ART: give the Activity a real app Context
      log_context(activity);
      // onCreate(null) -> onStart() -> onResume(), the documented launch order.
      result = call_lifecycle(cls, activity, "onCreate", "(Landroid/os/Bundle;)V", true);
      if (result.is_ok()) probe_managers(activity);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onStart", "()V", false);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onResume", "()V", false);
      // androidx ComponentActivity's LifecycleRegistry only reaches RESUMED via the framework's
      // Application.dispatchActivityResumed path, which acl's stub Application doesn't complete —
      // force it so lifecycle-gated Compose content (LaunchedEffect / collectAsStateWithLifecycle /
      // ViewModels) actually composes. Best-effort; ignore if HostBootstrap/androidx is absent.
      if (result.is_ok()) {
        if (jclass boot = env_->FindClass("org/acl/internal/HostBootstrap")) {
          if (jmethodID er = env_->GetStaticMethodID(boot, "ensureResumed", "(Ljava/lang/Object;)V"))
            env_->CallStaticVoidMethod(boot, er, activity);
          env_->DeleteLocalRef(boot);
        }
        clear_exception();
      }
      if (result.is_ok()) render_content_view(activity);
      env_->DeleteLocalRef(activity);
    }
    env_->DeleteLocalRef(activity_cls);
    env_->DeleteLocalRef(cls);
    return result;
  }

  // Run the app's Application (its onCreate) once, BEFORE the launcher Activity is instantiated —
  // the documented Android order. An Activity constructor / field initializer can read state the
  // Application sets, e.g. Telegram's LaunchActivity ctor builds a PiP controller that reads
  // ApplicationLoader.applicationContext (set by ApplicationLoaderImpl.onCreate) via
  // getSharedPreferences — a null context there NPEs the instantiation. Also pushes the theme +
  // display + color-scheme the app Resources need. Idempotent (createApplication runs once).
  void ensure_application() {
    jclass boot = env_->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) { clear_exception(); return; }
    if (jmethodID m = env_->GetStaticMethodID(boot, "setAppTheme", "(I)V"))
      env_->CallStaticVoidMethod(boot, m, static_cast<jint>(config_.application_theme));
    clear_exception();
    if (jmethodID m = env_->GetStaticMethodID(boot, "setDisplay", "(III)V")) {
      jint dw = static_cast<jint>(svc::window().display_width());
      jint dh = static_cast<jint>(svc::window().display_height());
      const jint ddpi = static_cast<jint>(svc::window().density_dpi());
      // ACL_DISPLAY_SIZE=WxH overrides the advertised display (hence the window/surface) size — the
      // app measures its content view to this, so the whole WebView/present path runs at WxH instead
      // of fullscreen (small-window / automotive cases, and reduced-size testing). e.g. 640x480.
      // NB: this does NOT speed up a network-bound WebView (map-tile zoom latency is Blink fetch time,
      // measured independent of surface area), only the per-frame pixel/raster cost.
      if (const char* e = std::getenv("ACL_DISPLAY_SIZE"); e != nullptr && *e != '\0') {
        int ew = 0, eh = 0;
        if (std::sscanf(e, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) {
          log::i(kTag, "ACL_DISPLAY_SIZE override: {}x{} (was {}x{})", ew, eh, dw, dh);
          dw = ew;
          dh = eh;
        } else {
          log::w(kTag, "ACL_DISPLAY_SIZE ignored (expected WxH, e.g. 640x480): '{}'", e);
        }
      }
      env_->CallStaticVoidMethod(boot, m, dw, dh, ddpi);
    }
    clear_exception();
#if ACL_HAVE_SDBUS
    if (jmethodID m = env_->GetStaticMethodID(boot, "setColorScheme", "(I)V"))
      env_->CallStaticVoidMethod(boot, m, static_cast<jint>(acl::appearance::query_color_scheme()));
    clear_exception();
#endif
    if (app_application_ == nullptr) {
      const std::string apk = config_.classpath.empty() ? std::string() : config_.classpath.front();
      const char* fwres = std::getenv("ACL_ART_FRAMEWORK_RES");
      jstring j_apk = env_->NewStringUTF(apk.c_str());
      jstring j_fw = env_->NewStringUTF(fwres != nullptr ? fwres : "");
      jstring j_pkg = env_->NewStringUTF(config_.package_name.c_str());
      jstring j_app = env_->NewStringUTF(config_.application_name.c_str());
      jobject app = env_->CallStaticObjectMethod(
          boot,
          env_->GetStaticMethodID(boot, "createApplication",
                                  "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                                  "Ljava/lang/String;)Landroid/app/Application;"),
          j_app, j_apk, j_fw, j_pkg);
      if (env_->ExceptionCheck() == JNI_TRUE) {
        env_->ExceptionDescribe();
        env_->ExceptionClear();
        log::w(kTag, "app Application {} onCreate hit a wall (continuing without singleton)",
               config_.application_name);
      } else if (app != nullptr) {
        app_application_ = env_->NewGlobalRef(app);
        log::i(kTag, "app Application ready: {}",
               config_.application_name.empty() ? "(default)" : config_.application_name);
      }
      if (app != nullptr) env_->DeleteLocalRef(app);
      env_->DeleteLocalRef(j_app);
      env_->DeleteLocalRef(j_apk);
      env_->DeleteLocalRef(j_fw);
      env_->DeleteLocalRef(j_pkg);
    }
    env_->DeleteLocalRef(boot);
  }

  // Host ART: give the Activity a real app Context (its package + resources.arsc) and attach
  // it (mBase + mActivityInfo + PhoneWindow), via acl's runtime bootstrap. No-op on the shim
  // path (HostBootstrap absent) or if the app class/resources aren't available.
  void attach_app_context(jobject activity) {
    jclass boot = env_->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) { clear_exception(); return; }
    const std::string apk = config_.classpath.empty() ? std::string() : config_.classpath.front();
    const char* fwres = std::getenv("ACL_ART_FRAMEWORK_RES");
    jstring j_apk = env_->NewStringUTF(apk.c_str());
    jstring j_fw = env_->NewStringUTF(fwres != nullptr ? fwres : "");
    jstring j_pkg = env_->NewStringUTF(config_.package_name.c_str());

    // The app's manifest theme, applied to the app Context (so decor/layout inflation
    // resolves framework + app theme attributes). Set before any Context is built.
    if (jmethodID set_theme = env_->GetStaticMethodID(boot, "setAppTheme", "(I)V")) {
      env_->CallStaticVoidMethod(boot, set_theme,
                                 static_cast<jint>(config_.application_theme));
    }
    clear_exception();

    // The advertised display (svc::window(): wl_output mode + density), pushed before any
    // Context/Resources is built so android.view.Display, the Resources Configuration, and the
    // content-view measure all agree on one size. Previously HostBootstrap hardcoded 1920x1080@160,
    // which disagreed with the real present surface (e.g. a 1280x1440 portrait wl_output was told
    // it was a 1920x1080 landscape display).
    if (jmethodID set_disp = env_->GetStaticMethodID(boot, "setDisplay", "(III)V")) {
      env_->CallStaticVoidMethod(boot, set_disp,
                                 static_cast<jint>(svc::window().display_width()),
                                 static_cast<jint>(svc::window().display_height()),
                                 static_cast<jint>(svc::window().density_dpi()));
    }
    clear_exception();

#if ACL_HAVE_SDBUS
    // Day/night: the desktop's color-scheme preference (XDG Settings portal) → Configuration.uiMode,
    // so AppCompat FOLLOW_SYSTEM and apps pick the right theme instead of always light. Best-effort;
    // -1 (no portal) leaves the light default. Must run before the Resources Configuration is built.
    if (jmethodID set_cs = env_->GetStaticMethodID(boot, "setColorScheme", "(I)V")) {
      env_->CallStaticVoidMethod(boot, set_cs,
                                 static_cast<jint>(acl::appearance::query_color_scheme()));
    }
    clear_exception();
#endif

    // The app's Application, created once (its onCreate sets the app's singleton).
    if (app_application_ == nullptr) {
      jstring j_app = env_->NewStringUTF(config_.application_name.c_str());
      jobject app = env_->CallStaticObjectMethod(
          boot,
          env_->GetStaticMethodID(boot, "createApplication",
                                  "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                                  "Ljava/lang/String;)Landroid/app/Application;"),
          j_app, j_apk, j_fw, j_pkg);
      if (env_->ExceptionCheck() == JNI_TRUE) {
        env_->ExceptionDescribe();
        env_->ExceptionClear();
        log::w(kTag, "app Application {} onCreate hit a wall (continuing without singleton)",
               config_.application_name);
      } else if (app != nullptr) {
        app_application_ = env_->NewGlobalRef(app);
        log::i(kTag, "app Application ready: {}",
               config_.application_name.empty() ? "(default)" : config_.application_name);
      }
      if (app != nullptr) env_->DeleteLocalRef(app);
      env_->DeleteLocalRef(j_app);
    }

    jobject ctx = env_->CallStaticObjectMethod(
        boot,
        env_->GetStaticMethodID(
            boot, "newAppContext",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/content/Context;"),
        j_apk, j_fw, j_pkg);
    if (ctx != nullptr && env_->ExceptionCheck() != JNI_TRUE) {
      env_->CallStaticVoidMethod(
          boot,
          env_->GetStaticMethodID(
              boot, "attachActivityContext",
              "(Landroid/app/Activity;Landroid/content/Context;Landroid/app/Application;)V"),
          activity, ctx, app_application_);
    }
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();
      env_->ExceptionClear();
      log::w(kTag, "attach app context: failed (continuing)");
    } else {
      log::i(kTag, "attach app context: package={} resources bound", config_.package_name);
    }
    env_->DeleteLocalRef(j_apk);
    env_->DeleteLocalRef(j_fw);
    env_->DeleteLocalRef(j_pkg);
    if (ctx != nullptr) env_->DeleteLocalRef(ctx);
    env_->DeleteLocalRef(boot);
  }
  jobject app_application_ = nullptr;  // the app's Application, created once (host ART)

  // The media-browse API family a hosted service uses: the framework
  // android.service.media.MediaBrowserService, or the support-library
  // androidx.media.MediaBrowserServiceCompat (what most real Auto apps extend). The
  // methods are identical bar the package of the nested BrowserRoot/Result/Description.
  struct MbsApi {
    const char* label;
    bool framework;
    std::string browser_root;  // internal name of the BrowserRoot nested class
    std::string result_cls;    // internal name of the Result nested class
    std::string media_desc;    // internal name of MediaDescription(Compat)
  };
  bool media_browser_api(jclass cls, MbsApi& api) {
    struct Cand { const char* base; const char* label; bool fw;
                  const char* root; const char* res; const char* desc; };
    static const Cand cands[] = {
        {"android/service/media/MediaBrowserService", "MediaBrowserService", true,
         "android/service/media/MediaBrowserService$BrowserRoot",
         "android/service/media/MediaBrowserService$Result", "android/media/MediaDescription"},
        {"androidx/media/MediaBrowserServiceCompat", "MediaBrowserServiceCompat", false,
         "androidx/media/MediaBrowserServiceCompat$BrowserRoot",
         "androidx/media/MediaBrowserServiceCompat$Result", "androidx/media/MediaDescriptionCompat"},
    };
    for (const Cand& c : cands) {
      jclass base = env_->FindClass(c.base);
      if (base == nullptr) { clear_exception(); continue; }
      const bool ok = env_->IsAssignableFrom(cls, base) == JNI_TRUE;
      env_->DeleteLocalRef(base);
      if (ok) { api = {c.label, c.fw, c.root, c.res, c.desc}; return true; }
    }
    return false;
  }

  Error call_media_browser(std::string_view service_class) override {
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_media_browser before start()");
    }
    const std::string name = to_jni_name(service_class);
    jclass cls = env_->FindClass(name.c_str());
    if (cls == nullptr) return class_load_error("MediaBrowserService", service_class);
    MbsApi api;
    if (!media_browser_api(cls, api)) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      return Error::of(Status::NotApplicable,
                       std::string(service_class) + " is not a MediaBrowserService(Compat)");
    }

    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject svc = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (svc == nullptr) {
      result = fail_with_exception("instantiate " + std::string(service_class));
    } else {
      log::i(kTag, "hosting {} {}", api.label, service_class);
      log_context(svc);
#if ACL_PROFILE_AUTOMOTIVE
      hosts::media::MediaBridge::instance().set_audio_route(&audio_route_);
#endif
      result = call_lifecycle(cls, svc, "onCreate", "()V", false);
      if (result.is_ok()) probe_managers(svc);  // the app's MediaSession now shows up
      // Session read uses the framework MediaSession; androidx uses MediaSessionCompat
      // (a separate follow-on) — skip it there.
      if (result.is_ok() && api.framework) result = read_session(cls, svc);
      if (result.is_ok()) result = browse_root(cls, svc, api);
      env_->DeleteLocalRef(svc);
    }
    env_->DeleteLocalRef(cls);
    return result;
  }

  Error call_car_app(std::string_view service_class) override {
#if !ACL_PROFILE_AUTOMOTIVE
    (void)service_class;
    return Error::of(Status::NotApplicable, "CAL host is automotive-profile only");
#else
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_car_app before start()");
    }
    const std::string name = to_jni_name(service_class);
    jclass cls = env_->FindClass(name.c_str());
    if (cls == nullptr) return class_load_error("CarAppService", service_class);
    jclass cas_cls = env_->FindClass("androidx/car/app/CarAppService");
    if (cas_cls == nullptr || env_->IsAssignableFrom(cls, cas_cls) != JNI_TRUE) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      if (cas_cls != nullptr) env_->DeleteLocalRef(cas_cls);
      return Error::of(Status::NotApplicable,
                       std::string(service_class) + " is not a CarAppService");
    }
    env_->DeleteLocalRef(cas_cls);

    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject svc = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (svc == nullptr) {
      result = fail_with_exception("instantiate " + std::string(service_class));
    } else {
      log::i(kTag, "hosting CarAppService {}", service_class);
      result = render_car_app(cls, svc);
      env_->DeleteLocalRef(svc);
    }
    env_->DeleteLocalRef(cls);
    return result;
#endif
  }

  Error shutdown() override {
    if (jvm_ != nullptr) {
      jvm_->DestroyJavaVM();  // blocks until non-daemon threads finish
      jvm_ = nullptr;
      env_ = nullptr;
      log::i(kTag, "{} shutdown", backend_name(backend()));
    }
    return Error::ok();
  }

 private:
  void clear_exception() {
    if (env_ != nullptr && env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionClear();
    }
  }

  // If an exception is pending, return its toString() (e.g. "java.lang.
  // NoClassDefFoundError: androidx/media/MediaBrowserServiceCompat") and clear it;
  // else "". Lets a FindClass failure distinguish a genuinely-absent class from one
  // that is present but cannot be linked (a missing dependency in its hierarchy).
  std::string pending_exception_text() {
    if (env_ == nullptr || env_->ExceptionCheck() != JNI_TRUE) return {};
    jthrowable ex = env_->ExceptionOccurred();
    env_->ExceptionClear();
    if (ex == nullptr) return {};
    std::string out;
    jclass ex_cls = env_->GetObjectClass(ex);
    jmethodID to_string = env_->GetMethodID(ex_cls, "toString", "()Ljava/lang/String;");
    if (to_string != nullptr) {
      auto s = static_cast<jstring>(env_->CallObjectMethod(ex, to_string));
      if (env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();  // toString threw
      if (s != nullptr) { out = utf8(s); env_->DeleteLocalRef(s); }
    }
    env_->DeleteLocalRef(ex_cls);
    env_->DeleteLocalRef(ex);
    return out;
  }

  // Report a failed entry-class lookup: a link error (present-but-unloadable) is a
  // RuntimeError with the underlying exception; a truly absent class is NotFound.
  Error class_load_error(std::string_view kind, std::string_view name) {
    const std::string exc = pending_exception_text();
    if (!exc.empty())
      return Error::of(Status::RuntimeError,
                       "could not load " + std::string(kind) + " " + std::string(name) + ": " + exc);
    return Error::of(Status::NotFound, "class not found: " + std::string(name));
  }

  // If `obj` is an android.content.Context (Service/Activity now are), inject the
  // manifest package and log getPackageName() — proof the entry point functions as
  // a Context carrying the app's real identity (a stand-in for attachBaseContext of
  // a ContextImpl).
  void log_context(jobject obj) {
    jclass ctx_cls = env_->FindClass("android/content/Context");
    if (ctx_cls == nullptr) { clear_exception(); return; }
    if (env_->IsInstanceOf(obj, ctx_cls) == JNI_TRUE) {
      jclass obj_cls = env_->GetObjectClass(obj);
      if (!config_.package_name.empty()) {
        jmethodID setter =
            env_->GetMethodID(obj_cls, "aclSetPackage", "(Ljava/lang/String;)V");
        if (setter != nullptr) {
          jstring pkg = env_->NewStringUTF(config_.package_name.c_str());
          env_->CallVoidMethod(obj, setter, pkg);
          env_->DeleteLocalRef(pkg);
        } else {
          clear_exception();
        }
      }
      jmethodID gpn = env_->GetMethodID(obj_cls, "getPackageName", "()Ljava/lang/String;");
      if (gpn != nullptr) {
        auto p = static_cast<jstring>(env_->CallObjectMethod(obj, gpn));
        log::i(kTag, "entry point is a Context: getPackageName()={}",
               p != nullptr ? utf8(p) : "?");
        if (p != nullptr) env_->DeleteLocalRef(p);
      }
      env_->DeleteLocalRef(obj_cls);
    }
    env_->DeleteLocalRef(ctx_cls);
  }

  // Exercise the svc/-backed Managers via getSystemService on the app's Context —
  // the framework itself calls these. Proves ActivityManager / WindowManager+Display
  // / MediaSessionManager are wired; the MediaSession the app created shows up.
  void probe_managers(jobject ctx) {
    jclass ctx_cls = env_->GetObjectClass(ctx);
    jmethodID gss = env_->GetMethodID(ctx_cls, "getSystemService",
                                      "(Ljava/lang/String;)Ljava/lang/Object;");
    env_->DeleteLocalRef(ctx_cls);
    if (gss == nullptr) { clear_exception(); return; }

    const auto svc = [&](const char* name) -> jobject {
      jstring s = env_->NewStringUTF(name);
      jobject o = env_->CallObjectMethod(ctx, gss, s);
      env_->DeleteLocalRef(s);
      if (env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();
      return o;
    };
    const auto call_int = [&](jobject o, const char* method) -> int {
      jclass c = env_->GetObjectClass(o);
      jmethodID m = env_->GetMethodID(c, method, "()I");
      const int v = m != nullptr ? env_->CallIntMethod(o, m) : -1;
      if (m == nullptr) clear_exception();
      env_->DeleteLocalRef(c);
      return v;
    };

    if (jobject am = svc("activity")) {
      log::i(kTag, "ActivityManager: memoryClass={} MB", call_int(am, "getMemoryClass"));
      env_->DeleteLocalRef(am);
    }
    if (jobject wm = svc("window")) {
      jclass c = env_->GetObjectClass(wm);
      jmethodID gdd = env_->GetMethodID(c, "getDefaultDisplay", "()Landroid/view/Display;");
      jobject d = gdd != nullptr ? env_->CallObjectMethod(wm, gdd) : nullptr;
      if (d != nullptr) {
        log::i(kTag, "WindowManager: display={}x{}", call_int(d, "getWidth"),
               call_int(d, "getHeight"));
        env_->DeleteLocalRef(d);
      } else { clear_exception(); }
      env_->DeleteLocalRef(c);
      env_->DeleteLocalRef(wm);
    }
    if (jobject msm = svc("media_session")) {
      log::i(kTag, "MediaSessionManager: activeSessions={}",
             call_int(msm, "aclActiveSessionCount"));
      env_->DeleteLocalRef(msm);
    }
  }

  // Phase-2 ViewRoot traversal. If the Activity set a content View (setContentView),
  // run the measure -> layout -> draw pass ViewRootImpl.performTraversals drives —
  // against the WindowManager display metrics, into a Bitmap-backed Canvas — and
  // describe the resulting pixels. Proves View(Context) + a real measure/layout/draw
  // traversal over the app's view tree renders verifiable output (no compositor).
#if ACL_HAVE_GL
  // Stage 3 routing: does the decor tree host an android.view.SurfaceView (e.g. a GLSurfaceView)?
  bool has_surface_view(jobject view) {
    jclass boot = env_->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) { clear_exception(); return false; }
    bool r = false;
    if (jmethodID m = env_->GetStaticMethodID(boot, "hasSurfaceView", "(Landroid/view/View;)Z"))
      r = env_->CallStaticBooleanMethod(boot, m, view) == JNI_TRUE;
    clear_exception();
    env_->DeleteLocalRef(boot);
    return r;
  }

  // Looper-driven GL present (Flutter): hand the decor to PresentDriver.runGl, which runs
  // Looper.loop() so the real Choreographer dispatches Flutter's vsync callbacks (clocked by a
  // wp_presentation-armed timerfd), while acl builds the GL device + injects the Surface natively.
  // Returns false if runGl is unavailable (JVM backend / no PresentDriver) so the caller can fall
  // back to the self-rendering external loop.
  bool present_gl_looper(jobject view, int w, int h) {
    jclass pd = env_->FindClass("org/acl/internal/PresentDriver");
    if (pd == nullptr) { clear_exception(); return false; }
    jmethodID run = env_->GetStaticMethodID(
        pd, "runGl", "(Landroid/view/View;IILjava/lang/String;Ljava/lang/String;)V");
    if (run == nullptr) { clear_exception(); env_->DeleteLocalRef(pd); return false; }
    const std::string& pkg = config_.package_name;
    jstring title = env_->NewStringUTF(pkg.empty() ? "acl" : pkg.c_str());
    jstring appId = env_->NewStringUTF(pkg.empty() ? "org.acl.app" : pkg.c_str());
    log::i(kTag, "--present(gl): handing off to PresentDriver.runGl (looper owns dispatch + vsync)");
    env_->CallStaticVoidMethod(pd, run, view, w, h, title, appId);
    if (env_->ExceptionCheck() == JNI_TRUE) fail_with_exception("PresentDriver.runGl");
    if (title != nullptr) env_->DeleteLocalRef(title);
    if (appId != nullptr) env_->DeleteLocalRef(appId);
    env_->DeleteLocalRef(pd);
    return true;
  }

  // Inject a ready Surface into the SurfaceView (HostBootstrap reflection) so its holder's
  // surfaceCreated/Changed fire on the main thread and the app's GLThread unblocks.
  void inject_gl_surface(jobject view, int w, int h) {
    jclass boot = env_->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) { clear_exception(); return; }
    if (jmethodID m = env_->GetStaticMethodID(boot, "injectGlSurface", "(Landroid/view/View;II)V"))
      env_->CallStaticVoidMethod(boot, m, view, w, h);
    clear_exception();
    env_->DeleteLocalRef(boot);
  }

  // Present a GLSurfaceView app (Stage 3). The app's OWN GLThread renders GL; acl creates the
  // toplevel wl_surface + a wayland GL device on it (the toplevel IS the GL surface, fullscreen
  // first cut), injects a Surface into the SurfaceView so the GLThread unblocks, and pumps wl
  // events while the app renders + eglSwapBuffers on its thread.
  void present_glsurfaceview(jobject view, int w, int h) {
    jobject gview = env_->NewGlobalRef(view);
    const std::string pkg = config_.package_name.empty() ? "acl" : config_.package_name;

    // Input seam (resolved once, used on the present-loop thread = this thread): a compositor
    // pointer/key becomes a MotionEvent/KeyEvent dispatched to the decor, which routes to the
    // GLSurfaceView — where libGDX's AndroidInput listens. Touch hit-testing needs laid-out views,
    // so the configure hook measures/lays out the decor too.
    jclass vcls = env_->GetObjectClass(gview);
    jmethodID dispatch_touch = env_->GetMethodID(vcls, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
    jmethodID dispatch_key = env_->GetMethodID(vcls, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
    jmethodID measure = env_->GetMethodID(vcls, "measure", "(II)V");
    jmethodID layoutm = env_->GetMethodID(vcls, "layout", "(IIII)V");
    env_->DeleteLocalRef(vcls);
    jclass mecls = static_cast<jclass>(env_->NewGlobalRef(env_->FindClass("android/view/MotionEvent")));
    jmethodID me_obtain = env_->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
    jmethodID me_recycle = env_->GetMethodID(mecls, "recycle", "()V");
    jclass kecls = static_cast<jclass>(env_->NewGlobalRef(env_->FindClass("android/view/KeyEvent")));
    jmethodID ke_obtain = env_->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
    clear_exception();
    auto down_time = std::make_shared<std::uint64_t>(0);

    wm::GlPresentHooks hooks;
    hooks.configure = [this, gview, w, h, measure, layoutm](void* wl_display, void* wl_surface,
                                                            int cw, int ch) -> bool {
      std::string err;
      auto dev = gfx::GlDevice::CreateWayland(wl_display, &err);
      if (!dev) { log::w(kTag, "glsv: CreateWayland failed: {}", err); return false; }
      {
        // Publish the device + wake the GLThread parked in get_display, so its context is created
        // on THIS wayland device (matching the window surface it will make below).
        std::lock_guard<std::mutex> lk(g_gl_session.mtx);
        g_gl_session.device = std::move(dev);
        g_gl_session.wl_surface = wl_surface;
        g_gl_session.w = cw > 0 ? cw : w;
        g_gl_session.h = ch > 0 ? ch : h;
        g_gl_session.ready = true;
      }
      g_gl_session.cv.notify_all();
      log::i(kTag, "glsv: GL device on acl's wl_display ({}); injecting Surface", g_gl_session.device->renderer());
      inject_gl_surface(gview, g_gl_session.w, g_gl_session.h);  // -> surfaceCreated -> GLThread runs
      // Lay out the decor so dispatchTouchEvent hit-tests down to the GLSurfaceView.
      if (measure != nullptr && layoutm != nullptr) {
        const jint mask = 0x3 << 30;  // MeasureSpec.EXACTLY(size)
        env_->CallVoidMethod(gview, measure, (g_gl_session.w & ~mask) | (1 << 30),
                             (g_gl_session.h & ~mask) | (1 << 30));
        env_->CallVoidMethod(gview, layoutm, 0, 0, g_gl_session.w, g_gl_session.h);
        clear_exception();
      }
      return true;
    };
    // No render hook -> external mode: the app's GLThread renders + swaps; acl pumps events.
    hooks.teardown = [] {
      std::lock_guard<std::mutex> lk(g_gl_session.mtx);
      g_gl_session.window.reset();
      g_gl_session.device.reset();
      g_gl_session.wl_surface = nullptr;
      g_gl_session.ready = false;
    };

    wm::InputCallback input = [this, gview, mecls, me_obtain, me_recycle, dispatch_touch, kecls,
                               ke_obtain, dispatch_key, down_time](const wm::InputEvent& ev) {
      if (const auto* p = std::get_if<wm::PointerInput>(&ev)) {
        if (me_obtain == nullptr || dispatch_touch == nullptr) return;
        const jint action = p->action == wm::PointerInput::Action::Down ? 0
                          : p->action == wm::PointerInput::Action::Up   ? 1 : 2;
        if (action == 0) *down_time = present_now_ns();
        jobject me = env_->CallStaticObjectMethod(mecls, me_obtain, static_cast<jlong>(*down_time),
            static_cast<jlong>(present_now_ns()), action, static_cast<jfloat>(p->x),
            static_cast<jfloat>(p->y), 0);
        if (me != nullptr) {
          const jboolean handled = env_->CallBooleanMethod(gview, dispatch_touch, me);
          static int idiag = 0;
          if (idiag < 12) { log::i(kTag, "glsv input: pointer action={} ({},{}) handled={}",
                                   action, p->x, p->y, handled == JNI_TRUE); ++idiag; }
          if (me_recycle != nullptr) env_->CallVoidMethod(me, me_recycle);
          env_->DeleteLocalRef(me);
        }
        clear_exception();
      } else if (const auto* k = std::get_if<wm::KeyInput>(&ev)) {
        if (ke_obtain == nullptr || dispatch_key == nullptr) return;
        const jint action = k->action == wm::KeyInput::Action::Down ? 0 : 1;
        jobject ke = env_->CallStaticObjectMethod(kecls, ke_obtain, action,
                                                  acl_evdev_to_android_keycode(k->keycode));
        if (ke != nullptr) { env_->CallBooleanMethod(gview, dispatch_key, ke); env_->DeleteLocalRef(ke); }
        clear_exception();
      }
    };

    wm::present_gl_loop(w, h, pkg, pkg, hooks, input, config_.present_frames);
    env_->DeleteGlobalRef(gview);
    env_->DeleteGlobalRef(mecls);
    env_->DeleteGlobalRef(kecls);
  }
#endif

  void render_content_view(jobject activity) {
    jclass acls = env_->GetObjectClass(activity);
    jobject view = nullptr;
    // Prefer acl's helper (view-host demo); real Activities lack it — fall back to the
    // standard decor view: activity.getWindow().getDecorView().
    if (jmethodID gcv = env_->GetMethodID(acls, "aclContentView", "()Landroid/view/View;");
        gcv != nullptr) {
      view = env_->CallObjectMethod(activity, gcv);
    }
    clear_exception();
    if (view == nullptr) {
      if (jmethodID gw = env_->GetMethodID(acls, "getWindow", "()Landroid/view/Window;");
          gw != nullptr) {
        if (jobject window = env_->CallObjectMethod(activity, gw); window != nullptr) {
          jclass wcls = env_->GetObjectClass(window);
          if (jmethodID gdv =
                  env_->GetMethodID(wcls, "getDecorView", "()Landroid/view/View;");
              gdv != nullptr) {
            view = env_->CallObjectMethod(window, gdv);
          }
          env_->DeleteLocalRef(wcls);
          env_->DeleteLocalRef(window);
        }
      }
      clear_exception();
    }
    env_->DeleteLocalRef(acls);
    if (view == nullptr) { clear_exception(); return; }  // no content view set

    const int w = svc::window().display_width();
    const int h = svc::window().display_height();
    // MeasureSpec.makeMeasureSpec(size, EXACTLY): size in the low 30 bits, mode 1.
    const jint mask = 0x3 << 30;
    const jint wspec = (w & ~mask) | (1 << 30);
    const jint hspec = (h & ~mask) | (1 << 30);

    jclass vcls = env_->GetObjectClass(view);
    jmethodID measure = env_->GetMethodID(vcls, "measure", "(II)V");
    jmethodID gmw = env_->GetMethodID(vcls, "getMeasuredWidth", "()I");
    jmethodID gmh = env_->GetMethodID(vcls, "getMeasuredHeight", "()I");
    jmethodID layout = env_->GetMethodID(vcls, "layout", "(IIII)V");
    jmethodID draw = env_->GetMethodID(vcls, "draw", "(Landroid/graphics/Canvas;)V");
    env_->DeleteLocalRef(vcls);
    if (measure == nullptr || gmw == nullptr || gmh == nullptr || layout == nullptr ||
        draw == nullptr) {
      clear_exception();
      env_->DeleteLocalRef(view);
      return;
    }

    // Attach the tree to a window (real Android's ViewRootImpl.setView) before traversal —
    // dispatchAttachedToWindow across the tree, needed by views like Jetpack ComposeView.
    if (jclass boot = env_->FindClass("org/acl/internal/HostBootstrap"); boot != nullptr) {
      if (jmethodID att =
              env_->GetStaticMethodID(boot, "attachDecorToWindow", "(Landroid/view/View;)V"))
        env_->CallStaticVoidMethod(boot, att, view);
      if (env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();
      env_->DeleteLocalRef(boot);
    } else if (env_->ExceptionCheck() == JNI_TRUE) {
      // HostBootstrap is absent on the JVM backend (only the host-ART path provides it); the failed
      // FindClass left a pending NoClassDefFoundError that would otherwise poison the traversal.
      env_->ExceptionClear();
    }

    // GPU present: drive the Choreographer-pumped present loop directly — its per-frame
    // doFrame pumps the looper so async content composes (Jetpack Compose composes on the
    // frame clock). The loop measures/lays out/draws each frame, so do NOT measure/layout
    // here first: the ComposeView's measure/layout blocks on a composition that only runs
    // once the loop's doFrame pumps it (chicken-and-egg).
    if (config_.present) {
      if (const char* gpu = std::getenv("ACL_PRESENT_GPU"); gpu != nullptr && *gpu != '\0') {
#if ACL_HAVE_GL
        // GLSurfaceView (Stage 3): if the content hosts a SurfaceView, its own GLThread renders
        // GL to a wl_surface — route to the dedicated GL present (the toplevel IS the GL surface),
        // not the Skia View-tree present, which cannot show the GL content.
        if (has_surface_view(view)) {
          // Looper-driven GL present (Flutter needs the Choreographer ticked for vsync). Falls back
          // to the self-rendering external loop if PresentDriver.runGl is unavailable.
          if (!present_gl_looper(view, w, h)) present_glsurfaceview(view, w, h);
          env_->DeleteLocalRef(view);
          return;
        }
#endif
        // Default: the Looper-driven present (approach A). ACL_PRESENT_LEGACY selects the old
        // native poll loop (kept for comparison; it starves the Android Looper so Compose hangs).
        const char* legacy = std::getenv("ACL_PRESENT_LEGACY");
        if (legacy != nullptr && *legacy != '\0') present_content_loop_gpu(view, w, h);
        else present_content_looper(view, w, h);
        env_->DeleteLocalRef(view);
        return;
      }
    }

    // Headless / non-GPU path: one-shot measure → layout → draw into a Bitmap.
    env_->CallVoidMethod(view, measure, wspec, hspec);
    const int mw = env_->CallIntMethod(view, gmw);
    const int mh = env_->CallIntMethod(view, gmh);
    log::i(kTag, "ViewRoot: performTraversals measured={}x{}, laying out {}x{}", mw, mh, w, h);
    env_->CallVoidMethod(view, layout, 0, 0, mw, mh);

    // Bitmap b = Bitmap.createBitmap(w, h); Canvas c = new Canvas(b); view.draw(c);
    jclass bmp_cls = env_->FindClass("android/graphics/Bitmap");
    jclass canvas_cls = env_->FindClass("android/graphics/Canvas");
    jobject bmp = nullptr;
    if (bmp_cls != nullptr && canvas_cls != nullptr) {
      jmethodID cctor = env_->GetMethodID(canvas_cls, "<init>",
                                          "(Landroid/graphics/Bitmap;)V");
      if (cctor != nullptr) {
        bmp = create_bitmap(env_, bmp_cls, w, h);
        jobject canvas = bmp != nullptr ? env_->NewObject(canvas_cls, cctor, bmp) : nullptr;
        if (canvas == nullptr) {
          // Without a Canvas there is no draw and no present. Saying so beats a silent no-op run
          // that looks like success.
          if (env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();
          log::w(kTag, "content view not rendered: no Bitmap/Canvas ({}x{})", w, h);
        }
        if (canvas != nullptr) {
          env_->CallVoidMethod(view, draw, canvas);
          // The app's view tree just ran arbitrary framework code, which can throw — an unbound
          // graphics native is the usual one (P2.4). Check here: leaving it pending makes the NEXT
          // JNI call that throws abort the process with "unexpected pending exception", naming that
          // innocent call instead of this draw. Report what actually happened and carry on.
          if (env_->ExceptionCheck() == JNI_TRUE) {
            log::w(kTag, "View.draw threw; the drawn content is incomplete");
            env_->ExceptionDescribe();  // names the throw (e.g. an unimplemented Canvas native)
            env_->ExceptionClear();
          }
          // aclDescribe is a debug aid on acl's Bitmap shim; the real framework's Bitmap has no
          // such method, so this lookup legitimately fails there — clear it, or the pending
          // NoSuchMethodError trips ART's next "No pending exception expected" check.
          if (jmethodID describe = env_->GetMethodID(bmp_cls, "aclDescribe", "()V");
              describe != nullptr) {
            env_->CallVoidMethod(bmp, describe);
          } else if (env_->ExceptionCheck() == JNI_TRUE) {
            env_->ExceptionClear();
          }
          // Present the content view in a Wayland window (--present), driving a
          // Choreographer loop: each vsync tick fires the app's frame callbacks and
          // re-draws. No-op if no compositor / built without wcs. ACL_PRESENT_GPU routes
          // to the GPU-native dma-buf loop (View.draw on Ganesh Vulkan); default is the
          // mature shm loop (which also carries input/IME).
          if (config_.present) {
            const char* gpu = std::getenv("ACL_PRESENT_GPU");
            if (gpu != nullptr && *gpu != '\0') present_content_loop_gpu(view, w, h);
            else present_content_loop(view, bmp, canvas, w, h);
          }
          env_->DeleteLocalRef(canvas);
        }
      }
    }
    if (env_->ExceptionCheck() == JNI_TRUE) fail_with_exception("render content view");
    if (bmp != nullptr) env_->DeleteLocalRef(bmp);
    if (bmp_cls != nullptr) env_->DeleteLocalRef(bmp_cls);
    if (canvas_cls != nullptr) env_->DeleteLocalRef(canvas_cls);
    env_->DeleteLocalRef(view);
  }

  // Present the content view in a Wayland window via wm/ (wcs), driven by a
  // Choreographer-style loop: on each vsync tick the ViewRoot fires the app's
  // frame callbacks (android.view.Choreographer.aclDoFrame), re-runs the View
  // draw, and blits into the presenter's frame — the ViewRootImpl.performTraversals
  // + Choreographer path. A static app (no frame callbacks) shows one frame and
  // stops; an animating one runs to the frame budget (--present-frames) or close.
  // The Bitmap's native pointer IS the gfx::Surface the View draws into.
  void present_content_loop(jobject view, jobject bmp, jobject canvas, int w, int h) {
    if (!wm::wayland_available()) {
      log::i(kTag, "--present: no Wayland compositor reachable; frame not shown");
      return;
    }
    jclass bmp_cls = env_->GetObjectClass(bmp);
    jmethodID ptr_m = env_->GetMethodID(bmp_cls, "aclPtr", "()J");
    env_->DeleteLocalRef(bmp_cls);
    if (ptr_m == nullptr) {
      // aclPtr() is acl's Bitmap SHIM handing back the gfx::Surface the View drew into, which this
      // loop blits from. The REAL android.graphics.Bitmap has no such method, so this shm path
      // cannot serve a real app — it is the shim/demo path.
      //
      // That is not a gap to close HERE: blitting a real Bitmap's pixels through shm would be a
      // CPU copy, and the architecture is the opposite (ADR-4 / P2.4) — Skia renders through
      // Ganesh Vulkan into a dma-buf with a TILED format modifier, handed to the compositor for
      // direct scanout, zero copy, no detiling. That path exists and works: ACL_PRESENT_GPU=1
      // (present_content_looper), which real apps render through today. So point the way rather
      // than silently showing nothing.
      clear_exception();
      log::w(kTag,
             "--present: this Bitmap has no aclPtr(), so the shm present path (acl's Bitmap shim) "
             "cannot show a real app's frame. Use ACL_PRESENT_GPU=1 — the zero-copy tiled-dmabuf "
             "path Skia renders through. The app ran and drew; nothing will be shown.");
      return;
    }
    auto* surface = reinterpret_cast<const gfx::Surface*>(env_->CallLongMethod(bmp, ptr_m));
    if (surface == nullptr) {
      log::w(kTag, "--present: Bitmap.aclPtr() returned no surface; nothing will be shown");
      return;
    }

    jclass vcls = env_->GetObjectClass(view);
    jmethodID draw = env_->GetMethodID(vcls, "draw", "(Landroid/graphics/Canvas;)V");
    env_->DeleteLocalRef(vcls);

    // Choreographer.getInstance() + the per-frame pump. Optional: if the shim isn't
    // on the classpath we present a single static frame.
    jobject choreo = nullptr;
    jmethodID do_frame = nullptr;
    if (jclass ch_cls = env_->FindClass("android/view/Choreographer")) {
      if (jmethodID gi = env_->GetStaticMethodID(ch_cls, "getInstance",
                                                 "()Landroid/view/Choreographer;"))
        choreo = env_->CallStaticObjectMethod(ch_cls, gi);
      do_frame = env_->GetMethodID(ch_cls, "aclDoFrame", "(J)J");
      env_->DeleteLocalRef(ch_cls);
    }
    // android.view.MotionEvent.obtain + the view's dispatchTouchEvent, for pointer
    // input. Optional: absent shims => input is simply not delivered.
    jobject motionevent_cls = nullptr;
    jmethodID me_obtain = nullptr, me_recycle = nullptr, dispatch_touch = nullptr;
    if (jclass mecls = env_->FindClass("android/view/MotionEvent")) {
      me_obtain = env_->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
      me_recycle = env_->GetMethodID(mecls, "recycle", "()V");
      motionevent_cls = env_->NewGlobalRef(mecls);
      env_->DeleteLocalRef(mecls);
    }
    // android.view.KeyEvent.obtain + the view's dispatchKeyEvent, for key input.
    jobject keyevent_cls = nullptr;
    jmethodID ke_obtain = nullptr, dispatch_key = nullptr;
    if (jclass kecls = env_->FindClass("android/view/KeyEvent")) {
      ke_obtain = env_->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
      keyevent_cls = env_->NewGlobalRef(kecls);
      env_->DeleteLocalRef(kecls);
    }
    if (jclass vc = env_->GetObjectClass(view)) {
      dispatch_touch = env_->GetMethodID(vc, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
      dispatch_key = env_->GetMethodID(vc, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
      env_->DeleteLocalRef(vc);
    }
    clear_exception();

    // IME: if the focused view produces an InputConnection, enable Wayland text
    // input; committed strings from the compositor's IME are delivered to it via
    // InputConnection.commitText.
    jobject input_conn = nullptr;
    jmethodID commit_text = nullptr, set_composing = nullptr, delete_surrounding = nullptr;
    bool want_ime = false;
    {
      jclass vc = env_->GetObjectClass(view);
      jmethodID create_ic = vc != nullptr ? env_->GetMethodID(
          vc, "onCreateInputConnection",
          "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;") : nullptr;
      jclass ei_cls = env_->FindClass("android/view/inputmethod/EditorInfo");
      if (create_ic != nullptr && ei_cls != nullptr) {
        jmethodID ei_ctor = env_->GetMethodID(ei_cls, "<init>", "()V");
        jobject ei = ei_ctor != nullptr ? env_->NewObject(ei_cls, ei_ctor) : nullptr;
        jobject ic = ei != nullptr ? env_->CallObjectMethod(view, create_ic, ei) : nullptr;
        if (ic != nullptr) {
          input_conn = env_->NewGlobalRef(ic);
          if (jclass ic_cls = env_->GetObjectClass(ic)) {
            commit_text = env_->GetMethodID(ic_cls, "commitText", "(Ljava/lang/CharSequence;I)Z");
            set_composing = env_->GetMethodID(ic_cls, "setComposingText", "(Ljava/lang/CharSequence;I)Z");
            delete_surrounding = env_->GetMethodID(ic_cls, "deleteSurroundingText", "(II)Z");
            env_->DeleteLocalRef(ic_cls);
          }
          want_ime = commit_text != nullptr;
          env_->DeleteLocalRef(ic);
        }
        if (ei != nullptr) env_->DeleteLocalRef(ei);
      }
      if (vc != nullptr) env_->DeleteLocalRef(vc);
      if (ei_cls != nullptr) env_->DeleteLocalRef(ei_cls);
      clear_exception();
    }

    const std::string& pkg = config_.package_name;
    const auto render = [&](gfx::Surface& out, std::uint64_t frame_time_ns,
                            std::uint64_t /*idx*/) -> bool {
      if (choreo != nullptr && do_frame != nullptr)
        env_->CallLongMethod(choreo, do_frame, static_cast<jlong>(frame_time_ns));
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("Choreographer.doFrame"); return false; }
      if (draw != nullptr) env_->CallVoidMethod(view, draw, canvas);  // re-draw with updated state
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("View.draw"); return false; }
      out.blit(surface->data());
      return true;  // present until the frame budget / window close
    };
    // Deliver a pointer as a MotionEvent and a key as a KeyEvent into the content view.
    std::uint64_t down_time = 0;
    const auto on_pointer = [&, this](const wm::PointerInput& ev) {
      if (motionevent_cls == nullptr || me_obtain == nullptr || dispatch_touch == nullptr) return;
      const jint action = ev.action == wm::PointerInput::Action::Down ? 0
                        : ev.action == wm::PointerInput::Action::Up   ? 1 : 2;  // DOWN/UP/MOVE
      if (action == 0) down_time = frame_index_now();
      const jlong t = static_cast<jlong>(frame_index_now());
      jobject me = env_->CallStaticObjectMethod(
          static_cast<jclass>(motionevent_cls), me_obtain, static_cast<jlong>(down_time), t,
          action, static_cast<jfloat>(ev.x), static_cast<jfloat>(ev.y), 0);
      if (me != nullptr) {
        env_->CallBooleanMethod(view, dispatch_touch, me);
        if (me_recycle != nullptr) env_->CallVoidMethod(me, me_recycle);
        env_->DeleteLocalRef(me);
      }
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    const auto on_key = [&, this](const wm::KeyInput& ev) {
      if (keyevent_cls == nullptr || ke_obtain == nullptr || dispatch_key == nullptr) return;
      const jint action = ev.action == wm::KeyInput::Action::Down ? 0 : 1;  // ACTION_DOWN/UP
      jobject ke = env_->CallStaticObjectMethod(static_cast<jclass>(keyevent_cls), ke_obtain,
                                                action, android_keycode(ev.keycode));
      if (ke != nullptr) {
        env_->CallBooleanMethod(view, dispatch_key, ke);
        env_->DeleteLocalRef(ke);
      }
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    // Route IME text through the app's InputConnection: preedit -> setComposingText
    // (composing), a final commit -> commitText.
    const auto on_text = [&, this](const wm::TextInput& ev) {
      const jmethodID m = ev.preedit ? set_composing : commit_text;
      log::i(kTag, "IME {}: \"{}\"", ev.preedit ? "preedit" : "committed", ev.text);
      if (input_conn == nullptr || m == nullptr) return;
      jstring s = env_->NewStringUTF(ev.text.c_str());
      env_->CallBooleanMethod(input_conn, m, s, ev.cursor);
      if (s != nullptr) env_->DeleteLocalRef(s);
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    const auto on_delete = [&, this](const wm::DeleteSurroundingText& ev) {
      log::i(kTag, "IME delete-surrounding: before={} after={}", ev.before, ev.after);
      if (input_conn == nullptr || delete_surrounding == nullptr) return;
      env_->CallBooleanMethod(input_conn, delete_surrounding,
                              static_cast<jint>(ev.before), static_cast<jint>(ev.after));
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    const wm::InputCallback input = [&](const wm::InputEvent& ev) {
      if (const auto* p = std::get_if<wm::PointerInput>(&ev)) on_pointer(*p);
      else if (const auto* k = std::get_if<wm::KeyInput>(&ev)) on_key(*k);
      else if (const auto* t = std::get_if<wm::TextInput>(&ev)) on_text(*t);
      else if (const auto* d = std::get_if<wm::DeleteSurroundingText>(&ev)) on_delete(*d);
    };
    wm::present_loop(w, h, pkg.empty() ? "acl" : pkg,
                     pkg.empty() ? "org.acl.app" : pkg, render, input, want_ime,
                     config_.present_frames);
    if (choreo != nullptr) env_->DeleteLocalRef(choreo);
    if (motionevent_cls != nullptr) env_->DeleteGlobalRef(motionevent_cls);
    if (keyevent_cls != nullptr) env_->DeleteGlobalRef(keyevent_cls);
    if (input_conn != nullptr) env_->DeleteGlobalRef(input_conn);
  }

  // S4b step 2: present the content view GPU-native. Each vsync tick, the app's
  // View.draw runs on a real android.graphics.Canvas backed by the Ganesh Vulkan
  // surface (begin_frame → Choreographer.doFrame → View.draw → end_frame), and the
  // resulting modifier-tiled dma-buf is presented via zwp_linux_dmabuf_v1 — no CPU
  // upload. Pointer/key input is delivered to the View (via present_dmabuf_loop's
  // wl_seat wiring); IME on this path is a follow-up. Gated by ACL_PRESENT_GPU so the
  // mature interactive shm loop stays the default.
 protected:  // ArtVm's view-host demo drives this directly
  // Looper-driven present (approach A, default): hand the decor View to Java's PresentDriver,
  // which runs Looper.loop() so the Android main thread owns Wayland dispatch and the real
  // Choreographer drives frames — unblocking Handler-posted async work (Jetpack Compose).
  void present_content_looper(jobject view, int w, int h) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    jclass pd = env_->FindClass("org/acl/internal/PresentDriver");
    if (pd == nullptr) {
      clear_exception();
      log::w(kTag, "--present(gpu): PresentDriver not on the boot classpath; falling back to loop");
      present_content_loop_gpu(view, w, h);
      return;
    }
    jmethodID run = env_->GetStaticMethodID(
        pd, "run", "(Landroid/view/View;IILjava/lang/String;Ljava/lang/String;)V");
    if (run == nullptr) {
      clear_exception();
      env_->DeleteLocalRef(pd);
      present_content_loop_gpu(view, w, h);
      return;
    }
    const std::string& pkg = config_.package_name;
    jstring title = env_->NewStringUTF(pkg.empty() ? "acl" : pkg.c_str());
    jstring appId = env_->NewStringUTF(pkg.empty() ? "org.acl.app" : pkg.c_str());
    log::i(kTag, "--present(gpu): handing off to PresentDriver (looper owns dispatch)");
    env_->CallStaticVoidMethod(pd, run, view, w, h, title, appId);
    if (env_->ExceptionCheck() == JNI_TRUE) fail_with_exception("PresentDriver.run");
    if (title != nullptr) env_->DeleteLocalRef(title);
    if (appId != nullptr) env_->DeleteLocalRef(appId);
    env_->DeleteLocalRef(pd);
#else
    (void)view; (void)w; (void)h;
#endif
  }

  void present_content_loop_gpu(jobject view, int w, int h) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    log::i(kTag, "--present(gpu): entered {}x{}, checking compositor…", w, h);
    if (!wm::wayland_available()) {
      log::i(kTag, "--present(gpu): no Wayland compositor reachable; frame not shown");
      return;
    }
    struct AclDmabuf {
      int fd;
      std::uint32_t width, height, plane_count;
      std::uint32_t offsets[4], strides[4];
      std::uint64_t modifier;
      std::uint32_t drm_format;
    };
    auto begin = reinterpret_cast<long (*)(int, int)>(
        ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
    auto finish = reinterpret_cast<bool (*)(int, AclDmabuf*)>(
        ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
    if (begin == nullptr || finish == nullptr) {
      log::w(kTag, "--present(gpu): begin/end frame missing (libacl_graphics_jni?)");
      return;
    }
    // Wrap the persistent GPU SkSurface canvas once; begin_frame clears it each tick.
    log::i(kTag, "--present(gpu): compositor ok, begin_frame…");
    const jlong ch = begin(w, h);
    if (ch == 0) { log::w(kTag, "--present(gpu): begin_frame failed"); return; }
    log::i(kTag, "--present(gpu): begin_frame ok (ch={}), setting up loop…", ch);
    jclass canvasC = env_->FindClass("android/graphics/Canvas");
    jmethodID cctor = canvasC ? env_->GetMethodID(canvasC, "<init>", "(J)V") : nullptr;
    jobject canvas = cctor ? env_->NewObject(canvasC, cctor, ch) : nullptr;
    if (canvas == nullptr) { clear_exception(); log::w(kTag, "--present(gpu): Canvas(long) failed"); return; }
    jclass vcls = env_->GetObjectClass(view);
    jmethodID draw = env_->GetMethodID(vcls, "draw", "(Landroid/graphics/Canvas;)V");
    // Measure/layout each frame (after the doFrame pump) so async content — Jetpack Compose
    // composes on the frame clock — is sized before it draws. MeasureSpec EXACTLY(w/h).
    jmethodID measure = env_->GetMethodID(vcls, "measure", "(II)V");
    jmethodID gmw = env_->GetMethodID(vcls, "getMeasuredWidth", "()I");
    jmethodID gmh = env_->GetMethodID(vcls, "getMeasuredHeight", "()I");
    jmethodID layout = env_->GetMethodID(vcls, "layout", "(IIII)V");
    env_->DeleteLocalRef(vcls);
    const jint ms_mask = 0x3 << 30;
    const jint wspec = (w & ~ms_mask) | (1 << 30);
    const jint hspec = (h & ~ms_mask) | (1 << 30);
    // Pump the main Looper each frame — async framework work (Jetpack Compose's recomposer)
    // posts to the Handler queue, which this native frame loop otherwise never runs.
    jclass boot_cls = env_->FindClass("org/acl/internal/HostBootstrap");
    jmethodID pump = boot_cls ? env_->GetStaticMethodID(boot_cls, "pumpMainLooper", "(I)V") : nullptr;
    if (boot_cls == nullptr) clear_exception();

    // Optional Choreographer pump (acl's shim exposes aclDoFrame). On host ART the REAL
    // Choreographer is present: its getInstance() needs a vsync native (DisplayEventReceiver)
    // and has no aclDoFrame — so tolerate both (clear the exception, skip it). The loop still
    // re-draws the View each vsync tick; a static View just repaints identically.
    jobject choreo = nullptr;
    jmethodID do_frame = nullptr;
    if (jclass ch_cls = env_->FindClass("android/view/Choreographer")) {
      if (jmethodID gi = env_->GetStaticMethodID(ch_cls, "getInstance",
                                                 "()Landroid/view/Choreographer;")) {
        choreo = env_->CallStaticObjectMethod(ch_cls, gi);
        if (env_->ExceptionCheck() == JNI_TRUE) { env_->ExceptionClear(); choreo = nullptr; }
      }
      if (choreo != nullptr) do_frame = env_->GetMethodID(ch_cls, "aclDoFrame", "(J)J");
      env_->DeleteLocalRef(ch_cls);
    }
    clear_exception();

    // Input: pointer -> MotionEvent.dispatchTouchEvent, key -> KeyEvent.dispatchKeyEvent
    // (same mapping as the shm present_content_loop; IME on this path is a follow-up).
    jobject motionevent_cls = nullptr, keyevent_cls = nullptr;
    jmethodID me_obtain = nullptr, me_recycle = nullptr, ke_obtain = nullptr;
    jmethodID dispatch_touch = nullptr, dispatch_key = nullptr;
    if (jclass mecls = env_->FindClass("android/view/MotionEvent")) {
      me_obtain = env_->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
      me_recycle = env_->GetMethodID(mecls, "recycle", "()V");
      motionevent_cls = env_->NewGlobalRef(mecls);
      env_->DeleteLocalRef(mecls);
    }
    if (jclass kecls = env_->FindClass("android/view/KeyEvent")) {
      ke_obtain = env_->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
      keyevent_cls = env_->NewGlobalRef(kecls);
      env_->DeleteLocalRef(kecls);
    }
    if (jclass vc = env_->GetObjectClass(view)) {
      dispatch_touch = env_->GetMethodID(vc, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
      dispatch_key = env_->GetMethodID(vc, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
      env_->DeleteLocalRef(vc);
    }
    clear_exception();
    std::uint64_t down_time = 0;
    const wm::InputCallback input = [&, this](const wm::InputEvent& ev) {
      if (const auto* p = std::get_if<wm::PointerInput>(&ev)) {
        if (motionevent_cls == nullptr || me_obtain == nullptr || dispatch_touch == nullptr) return;
        const jint action = p->action == wm::PointerInput::Action::Down ? 0
                          : p->action == wm::PointerInput::Action::Up   ? 1 : 2;
        if (action == 0) down_time = frame_index_now();
        jobject me = env_->CallStaticObjectMethod(
            static_cast<jclass>(motionevent_cls), me_obtain, static_cast<jlong>(down_time),
            static_cast<jlong>(frame_index_now()), action, static_cast<jfloat>(p->x),
            static_cast<jfloat>(p->y), 0);
        if (me != nullptr) {
          env_->CallBooleanMethod(view, dispatch_touch, me);
          if (me_recycle != nullptr) env_->CallVoidMethod(me, me_recycle);
          env_->DeleteLocalRef(me);
        }
        if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
      } else if (const auto* k = std::get_if<wm::KeyInput>(&ev)) {
        if (keyevent_cls == nullptr || ke_obtain == nullptr || dispatch_key == nullptr) return;
        const jint action = k->action == wm::KeyInput::Action::Down ? 0 : 1;
        jobject ke = env_->CallStaticObjectMethod(static_cast<jclass>(keyevent_cls), ke_obtain,
                                                  action, android_keycode(k->keycode));
        if (ke != nullptr) {
          env_->CallBooleanMethod(view, dispatch_key, ke);
          env_->DeleteLocalRef(ke);
        }
        if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
      }
    };

    const wm::DmabufFrameFn render = [&](int bufidx, const std::vector<std::uint64_t>&,
                                         std::uint64_t frame_time_ns, std::uint64_t idx,
                                         gfx::DmabufFrame* out) -> bool {
      if (idx > 0 && begin(w, h) == 0) return false;  // re-arm + clear (frame 0 pre-armed)
      if (choreo != nullptr && do_frame != nullptr)
        env_->CallLongMethod(choreo, do_frame, static_cast<jlong>(frame_time_ns));
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("Choreographer.doFrame"); return false; }
      // Run pending Handler work (Compose recomposition) posted by doFrame, before measure.
      if (pump != nullptr && boot_cls != nullptr) {
        env_->CallStaticVoidMethod(boot_cls, pump, static_cast<jint>(256));
        if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
      }
      // measure → layout → draw (post-doFrame, so Compose has composed before it lays out).
      if (measure != nullptr && layout != nullptr) {
        env_->CallVoidMethod(view, measure, wspec, hspec);
        const int mw = env_->CallIntMethod(view, gmw);
        const int mh = env_->CallIntMethod(view, gmh);
        env_->CallVoidMethod(view, layout, 0, 0, mw, mh);
        if (idx == 0) log::i(kTag, "--present(gpu): frame0 measured {}x{}", mw, mh);
      }
      if (draw != nullptr) env_->CallVoidMethod(view, draw, canvas);
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("View.draw"); return false; }
      AclDmabuf f{};
      if (!finish(bufidx, &f) || f.fd < 0) return false;
      gfx::DmabufFrame fr{};
      fr.fd = f.fd;
      fr.width = f.width;
      fr.height = f.height;
      fr.plane_count = f.plane_count;
      for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
        fr.planes[i].offset = f.offsets[i];
        fr.planes[i].stride = f.strides[i];
      }
      fr.modifier = f.modifier;
      fr.drm_format = f.drm_format;
      fr.modifier_tiled = f.modifier != 0;
      *out = fr;
      return true;
    };
    const std::string& pkg = config_.package_name;
    log::i(kTag, "--present(gpu): starting present_dmabuf_loop (budget={})", config_.present_frames);
    const bool ok = wm::present_dmabuf_loop(w, h, pkg.empty() ? "acl" : pkg,
                                            pkg.empty() ? "org.acl.app" : pkg, render, input,
                                            /*want_ime=*/false, config_.present_frames);
    log::i(kTag, "--present(gpu): View.draw → Ganesh Vulkan → dma-buf → Wayland {} (zero-copy)",
           ok ? "presented" : "did not present");
    if (choreo != nullptr) env_->DeleteLocalRef(choreo);
    if (canvas != nullptr) env_->DeleteLocalRef(canvas);
    if (canvasC != nullptr) env_->DeleteLocalRef(canvasC);
    if (motionevent_cls != nullptr) env_->DeleteGlobalRef(motionevent_cls);
    if (keyevent_cls != nullptr) env_->DeleteGlobalRef(keyevent_cls);
#else
    (void)view; (void)w; (void)h;
#endif
  }

 private:
  // Map a Linux evdev scancode to an Android KeyEvent keycode (a small practical
  // subset; unmapped keys become KEYCODE_UNKNOWN=0). Mirrors Android's Generic.kl.
  static jint android_keycode(std::uint32_t evdev) {
    switch (evdev) {
      case 30: return 29;  case 48: return 30;  case 46: return 31;  case 32: return 32;  // A B C D
      case 18: return 33;  case 33: return 34;  case 34: return 35;  case 35: return 36;  // E F G H
      case 23: return 37;  case 36: return 38;  case 37: return 39;  case 38: return 40;  // I J K L
      case 50: return 41;  case 49: return 42;  case 24: return 43;  case 25: return 44;  // M N O P
      case 16: return 45;  case 19: return 46;  case 31: return 47;  case 20: return 48;  // Q R S T
      case 22: return 49;  case 47: return 50;  case 17: return 51;  case 45: return 52;  // U V W X
      case 21: return 53;  case 44: return 54;                                            // Y Z
      case 2: return 8;   case 3: return 9;   case 4: return 10;  case 5: return 11;       // 1 2 3 4
      case 6: return 12;  case 7: return 13;  case 8: return 14;  case 9: return 15;       // 5 6 7 8
      case 10: return 16; case 11: return 7;                                               // 9 0
      case 28: return 66;  // ENTER
      case 57: return 62;  // SPACE
      case 1: return 111;  // ESC -> KEYCODE_ESCAPE
      case 14: return 67;  // BACKSPACE -> KEYCODE_DEL
      case 15: return 61;  // TAB
      case 105: return 21; case 106: return 22;  // LEFT RIGHT
      case 103: return 19; case 108: return 20;  // UP DOWN
      default: return 0;   // KEYCODE_UNKNOWN
    }
  }

  // A monotonic timestamp (ms-ish) for MotionEvent times; wall-clock isn't needed.
  static std::uint64_t frame_index_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
  }

  // Print + clear a pending exception and return a RuntimeError describing `what`.
  Error fail_with_exception(const std::string& what) {
    if (env_->ExceptionCheck() == JNI_TRUE) {
      jthrowable t = env_->ExceptionOccurred();
      env_->ExceptionDescribe();  // stack to stderr (often empty for linker-thrown errors)
      env_->ExceptionClear();
      log_throwable_chain(t);     // class + message + cause chain (names the missing class)
      if (t != nullptr) env_->DeleteLocalRef(t);
    }
    return Error::of(Status::RuntimeError, "failed to " + what);
  }

  // Log a throwable's type, message, and cause chain. A NoClassDefFoundError thrown by the
  // class linker often carries an empty stack but a message (or a ClassNotFoundException cause)
  // that names the missing class — the only place the culprit surfaces.
  void log_throwable_chain(jthrowable t) {
    jclass thr_cls = env_->FindClass("java/lang/Throwable");
    jclass cls_cls = env_->FindClass("java/lang/Class");
    if (thr_cls == nullptr || cls_cls == nullptr) { clear_exception(); return; }
    jmethodID m_getMessage = env_->GetMethodID(thr_cls, "getMessage", "()Ljava/lang/String;");
    jmethodID m_getCause = env_->GetMethodID(thr_cls, "getCause", "()Ljava/lang/Throwable;");
    jmethodID m_getName = env_->GetMethodID(cls_cls, "getName", "()Ljava/lang/String;");
    jthrowable cur = t;   // depth 0 is the caller-owned ref; causes below are ours to free
    bool owned = false;
    for (int depth = 0; cur != nullptr && depth < 8; ++depth) {
      jclass tc = env_->GetObjectClass(cur);
      auto jname = static_cast<jstring>(env_->CallObjectMethod(tc, m_getName));
      auto jmsg = static_cast<jstring>(env_->CallObjectMethod(cur, m_getMessage));
      const char* cname = jname != nullptr ? env_->GetStringUTFChars(jname, nullptr) : nullptr;
      const char* cmsg = jmsg != nullptr ? env_->GetStringUTFChars(jmsg, nullptr) : nullptr;
      log::w(kTag, "  {}: {} -- {}", depth == 0 ? "exception" : "caused by",
             cname != nullptr ? cname : "?", cmsg != nullptr ? cmsg : "(no message)");
      if (cname != nullptr) env_->ReleaseStringUTFChars(jname, cname);
      if (cmsg != nullptr) env_->ReleaseStringUTFChars(jmsg, cmsg);
      if (jname != nullptr) env_->DeleteLocalRef(jname);
      if (jmsg != nullptr) env_->DeleteLocalRef(jmsg);
      env_->DeleteLocalRef(tc);
      jthrowable cause = static_cast<jthrowable>(env_->CallObjectMethod(cur, m_getCause));
      const bool stop = cause == nullptr || env_->IsSameObject(cause, cur) == JNI_TRUE;
      if (owned) env_->DeleteLocalRef(cur);
      if (stop) {
        if (cause != nullptr) env_->DeleteLocalRef(cause);
        break;
      }
      cur = cause;
      owned = true;
    }
    clear_exception();
    env_->DeleteLocalRef(thr_cls);
    env_->DeleteLocalRef(cls_cls);
  }

  // Call onGetRoot(pkg, uid, null) and read BrowserRoot.getRootId(). `api` supplies
  // the framework-vs-androidx nested-class names.
  Error browse_root(jclass cls, jobject svc, const MbsApi& api) {
    const std::string sig =
        "(Ljava/lang/String;ILandroid/os/Bundle;)L" + api.browser_root + ";";
    jmethodID on_get_root = env_->GetMethodID(cls, "onGetRoot", sig.c_str());
    if (on_get_root == nullptr) {
      clear_exception();
      return Error::of(Status::NotFound, "no onGetRoot(String,int,Bundle)");
    }
    jstring pkg = env_->NewStringUTF("acl");
    jobject root = env_->CallObjectMethod(svc, on_get_root, pkg, static_cast<jint>(1000),
                                          nullptr);
    env_->DeleteLocalRef(pkg);
    if (env_->ExceptionCheck() == JNI_TRUE) return fail_with_exception("onGetRoot()");
    if (root == nullptr) {
      return Error::of(Status::RuntimeError, "onGetRoot returned null (browse rejected)");
    }
    jclass root_cls = env_->GetObjectClass(root);
    jmethodID get_root_id =
        env_->GetMethodID(root_cls, "getRootId", "()Ljava/lang/String;");
    Error result = Error::ok();
    if (get_root_id == nullptr) {
      clear_exception();
      result = Error::of(Status::NotFound, "no BrowserRoot.getRootId()");
    } else {
      auto id = static_cast<jstring>(env_->CallObjectMethod(root, get_root_id));
      const char* c = id != nullptr ? env_->GetStringUTFChars(id, nullptr) : nullptr;
      log::i(kTag, "media browse root: {}", c != nullptr ? c : "?");
      if (c != nullptr) env_->ReleaseStringUTFChars(id, c);
      result = load_children(cls, svc, id, api);  // walk the root's children
      if (id != nullptr) env_->DeleteLocalRef(id);
    }
    env_->DeleteLocalRef(root_cls);
    env_->DeleteLocalRef(root);
    return result;
  }

  // Call onLoadChildren(parentId, new Result()) and enumerate the MediaItems the
  // app sent — their titles (in-process, no Binder). A String utf8 accessor.
  std::string utf8(jstring s) {
    if (s == nullptr) return {};
    const char* c = env_->GetStringUTFChars(s, nullptr);
    std::string out(c != nullptr ? c : "");
    if (c != nullptr) env_->ReleaseStringUTFChars(s, c);
    return out;
  }

  // MediaMetadata.getString(key), or `dflt` when the key is absent/null.
  std::string metadata_string(jobject md, jmethodID get_string, const char* key,
                              const char* dflt) {
    jstring k = env_->NewStringUTF(key);
    auto v = static_cast<jstring>(env_->CallObjectMethod(md, get_string, k));
    std::string out = v != nullptr ? utf8(v) : std::string(dflt);
    if (v != nullptr) env_->DeleteLocalRef(v);
    env_->DeleteLocalRef(k);
    return out;
  }

  Error load_children(jclass cls, jobject svc, jstring parent_id, const MbsApi& api) {
    const std::string on_load_sig = "(Ljava/lang/String;L" + api.result_cls + ";)V";
    jmethodID on_load = env_->GetMethodID(cls, "onLoadChildren", on_load_sig.c_str());
    if (on_load == nullptr) { clear_exception(); return Error::ok(); }  // optional

    jclass result_cls = env_->FindClass(api.result_cls.c_str());
    jmethodID result_ctor =
        result_cls != nullptr ? env_->GetMethodID(result_cls, "<init>", "()V") : nullptr;
    jobject result = result_ctor != nullptr ? env_->NewObject(result_cls, result_ctor) : nullptr;
    if (result == nullptr) { clear_exception(); return fail_with_exception("build Result"); }

    env_->CallVoidMethod(svc, on_load, parent_id, result);
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->DeleteLocalRef(result); env_->DeleteLocalRef(result_cls);
      return fail_with_exception("onLoadChildren()");
    }

    // list = result.aclValue()
    jobject list = env_->CallObjectMethod(
        result, env_->GetMethodID(result_cls, "aclValue", "()Ljava/lang/Object;"));
    Error result_err = Error::ok();
    if (list == nullptr) {
      log::i(kTag, "media children: (none sent)");
    } else {
      jclass list_cls = env_->FindClass("java/util/List");
      jmethodID size_m = env_->GetMethodID(list_cls, "size", "()I");
      jmethodID get_m = env_->GetMethodID(list_cls, "get", "(I)Ljava/lang/Object;");
      const jint n = env_->CallIntMethod(list, size_m);
      const std::string parent = utf8(parent_id);
      log::i(kTag, "media children of '{}': {} item(s)", parent, n);
      std::vector<std::string> titles;
      for (jint i = 0; i < n; ++i) {
        jobject item = env_->CallObjectMethod(list, get_m, i);  // MediaBrowser.MediaItem
        jclass item_cls = env_->GetObjectClass(item);
        const std::string desc_sig = "()L" + api.media_desc + ";";
        jobject desc = env_->CallObjectMethod(
            item, env_->GetMethodID(item_cls, "getDescription", desc_sig.c_str()));
        jclass desc_cls = env_->GetObjectClass(desc);
        auto title = env_->CallObjectMethod(  // CharSequence
            desc, env_->GetMethodID(desc_cls, "getTitle", "()Ljava/lang/CharSequence;"));
        std::string title_s = "(no title)";
        if (title != nullptr) {  // CharSequence.toString()
          jclass cs_cls = env_->GetObjectClass(title);
          auto s = static_cast<jstring>(env_->CallObjectMethod(
              title, env_->GetMethodID(cs_cls, "toString", "()Ljava/lang/String;")));
          title_s = utf8(s);
          if (s != nullptr) env_->DeleteLocalRef(s);
          env_->DeleteLocalRef(cs_cls);
          env_->DeleteLocalRef(title);
        }
        log::i(kTag, "  media item: {}", title_s);
        titles.push_back(title_s);
        env_->DeleteLocalRef(desc_cls); env_->DeleteLocalRef(desc);
        env_->DeleteLocalRef(item_cls); env_->DeleteLocalRef(item);
      }
      env_->DeleteLocalRef(list_cls);
      env_->DeleteLocalRef(list);
#if ACL_PROFILE_AUTOMOTIVE
      // Publish the browse tree snapshot northbound (Gate G1.5a).
      auto& bridge = hosts::media::MediaBridge::instance();
      bridge.publish_browse(parent, titles);
      log::i(kTag, "MediaBridge: browse snapshot root='{}' {} item(s)",
             bridge.browse_root(), bridge.browse_items().size());
#endif
    }
    env_->DeleteLocalRef(result);
    env_->DeleteLocalRef(result_cls);
    return result_err;
  }

#if ACL_PROFILE_AUTOMOTIVE
  // Walk a CarAppService -> Session -> Screen -> Template and render it through the
  // native CAL host (Gate G1.5b). In-process, no Binder (ADR-3).
  Error render_car_app(jclass cls, jobject svc) {
    jmethodID m_sess = env_->GetMethodID(cls, "onCreateSession",
                                         "()Landroidx/car/app/Session;");
    if (m_sess == nullptr) { clear_exception(); return Error::of(Status::NotFound, "no onCreateSession()"); }
    jobject session = env_->CallObjectMethod(svc, m_sess);
    if (env_->ExceptionCheck() == JNI_TRUE) return fail_with_exception("onCreateSession()");
    if (session == nullptr) return Error::of(Status::RuntimeError, "onCreateSession returned null");

    jclass sess_cls = env_->GetObjectClass(session);
    jmethodID m_screen = env_->GetMethodID(sess_cls, "onCreateScreen",
                                           "()Landroidx/car/app/Screen;");
    jobject screen = m_screen != nullptr ? env_->CallObjectMethod(session, m_screen) : nullptr;
    if (env_->ExceptionCheck() == JNI_TRUE) { env_->DeleteLocalRef(sess_cls); env_->DeleteLocalRef(session); return fail_with_exception("onCreateScreen()"); }
    if (screen == nullptr) { env_->DeleteLocalRef(sess_cls); env_->DeleteLocalRef(session); return Error::of(Status::RuntimeError, "onCreateScreen returned null"); }

    jclass screen_cls = env_->GetObjectClass(screen);
    jmethodID m_tmpl = env_->GetMethodID(screen_cls, "onGetTemplate",
                                         "()Landroidx/car/app/model/Template;");
    jobject tmpl = m_tmpl != nullptr ? env_->CallObjectMethod(screen, m_tmpl) : nullptr;
    Error result = Error::ok();
    if (env_->ExceptionCheck() == JNI_TRUE) {
      result = fail_with_exception("onGetTemplate()");
    } else if (tmpl == nullptr) {
      result = Error::of(Status::RuntimeError, "onGetTemplate returned null");
    } else {
      const hosts::cal::Template t = read_template(tmpl);
      auto& host = hosts::cal::CalHost::instance();
      host.render(t);
      log::i(kTag, "CalHost: rendered [{}] '{}' — {} row(s), limit {}, truncated={}",
             hosts::cal::template_kind_name(t.kind), t.title, host.visible_rows(),
             host.content_limit(), host.truncated());
      for (const std::string& line : host.lines()) log::i(kTag, "  {}", line);
      env_->DeleteLocalRef(tmpl);
    }
    env_->DeleteLocalRef(screen_cls); env_->DeleteLocalRef(screen);
    env_->DeleteLocalRef(sess_cls); env_->DeleteLocalRef(session);
    return result;
  }

  // Deserialize a Template into the native model (Tier-1: ListTemplate). The shim's
  // acl* accessors substitute for the Bundleable read the host would do over Binder.
  hosts::cal::Template read_template(jobject tmpl) {
    hosts::cal::Template t;
    jclass lt_cls = env_->FindClass("androidx/car/app/model/ListTemplate");
    if (lt_cls == nullptr || env_->IsInstanceOf(tmpl, lt_cls) != JNI_TRUE) {
      clear_exception();
      if (lt_cls != nullptr) env_->DeleteLocalRef(lt_cls);
      t.kind = hosts::cal::TemplateKind::Unknown;
      log::w(kTag, "CAL template is not a Tier-1 ListTemplate; rendering header only");
      return t;
    }
    t.kind = hosts::cal::TemplateKind::List;
    jclass tcls = env_->GetObjectClass(tmpl);
    auto title = static_cast<jstring>(env_->CallObjectMethod(
        tmpl, env_->GetMethodID(tcls, "aclTitle", "()Ljava/lang/String;")));
    if (title != nullptr) { t.title = utf8(title); env_->DeleteLocalRef(title); }
    jobject list = env_->CallObjectMethod(
        tmpl, env_->GetMethodID(tcls, "aclList", "()Landroidx/car/app/model/ItemList;"));
    if (list != nullptr) {
      jclass il_cls = env_->GetObjectClass(list);
      jobject rows = env_->CallObjectMethod(
          list, env_->GetMethodID(il_cls, "aclRows", "()Ljava/util/List;"));
      if (rows != nullptr) {
        jclass list_cls = env_->FindClass("java/util/List");
        jmethodID size_m = env_->GetMethodID(list_cls, "size", "()I");
        jmethodID get_m = env_->GetMethodID(list_cls, "get", "(I)Ljava/lang/Object;");
        const jint n = env_->CallIntMethod(rows, size_m);
        for (jint i = 0; i < n; ++i) {
          jobject row = env_->CallObjectMethod(rows, get_m, i);
          jclass row_cls = env_->GetObjectClass(row);
          hosts::cal::Row r;
          auto rt = static_cast<jstring>(env_->CallObjectMethod(
              row, env_->GetMethodID(row_cls, "aclTitle", "()Ljava/lang/String;")));
          if (rt != nullptr) { r.title = utf8(rt); env_->DeleteLocalRef(rt); }
          auto rx = static_cast<jstring>(env_->CallObjectMethod(
              row, env_->GetMethodID(row_cls, "aclText", "()Ljava/lang/String;")));
          if (rx != nullptr) { r.text = utf8(rx); env_->DeleteLocalRef(rx); }
          t.rows.push_back(std::move(r));
          env_->DeleteLocalRef(row_cls); env_->DeleteLocalRef(row);
        }
        env_->DeleteLocalRef(list_cls); env_->DeleteLocalRef(rows);
      }
      env_->DeleteLocalRef(il_cls); env_->DeleteLocalRef(list);
    }
    env_->DeleteLocalRef(tcls); env_->DeleteLocalRef(lt_cls);
    return t;
  }
#endif  // ACL_PROFILE_AUTOMOTIVE

  // After onCreate, read the published MediaSession via getSessionToken() ->
  // Token -> MediaSession, and log its now-playing metadata + playback state
  // (in-process, no controller/Binder). No session is not an error.
  Error read_session(jclass cls, jobject svc) {
    jmethodID get_token = env_->GetMethodID(
        cls, "getSessionToken", "()Landroid/media/session/MediaSession$Token;");
    if (get_token == nullptr) { clear_exception(); return Error::ok(); }
    jobject token = env_->CallObjectMethod(svc, get_token);
    if (env_->ExceptionCheck() == JNI_TRUE) return fail_with_exception("getSessionToken()");
    if (token == nullptr) { return Error::ok(); }  // app published no session

    jclass token_cls = env_->GetObjectClass(token);
    jobject session = env_->CallObjectMethod(
        token, env_->GetMethodID(token_cls, "aclSession",
                                 "()Landroid/media/session/MediaSession;"));
    jclass ses_cls = session != nullptr ? env_->GetObjectClass(session) : nullptr;

    std::string title = "(none)";
    std::string artist;
    int state = -1;
    if (session != nullptr) {
      jobject md = env_->CallObjectMethod(
          session, env_->GetMethodID(ses_cls, "aclMetadata",
                                     "()Landroid/media/MediaMetadata;"));
      if (md != nullptr) {
        jclass md_cls = env_->GetObjectClass(md);
        jmethodID get_string = env_->GetMethodID(
            md_cls, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
        title = metadata_string(md, get_string, "android.media.metadata.TITLE", "(none)");
        artist = metadata_string(md, get_string, "android.media.metadata.ARTIST", "");
        env_->DeleteLocalRef(md_cls); env_->DeleteLocalRef(md);
      }
      jobject ps = env_->CallObjectMethod(
          session, env_->GetMethodID(ses_cls, "aclPlaybackState",
                                     "()Landroid/media/session/PlaybackState;"));
      if (ps != nullptr) {
        jclass ps_cls = env_->GetObjectClass(ps);
        state = env_->CallIntMethod(ps, env_->GetMethodID(ps_cls, "getState", "()I"));
        env_->DeleteLocalRef(ps_cls); env_->DeleteLocalRef(ps);
      }
    }
    log::i(kTag, "media session: title='{}' state={}", title, state);
#if ACL_PROFILE_AUTOMOTIVE
    // Publish northbound into the media bridge (Gate G1.5a) and report the MPRIS
    // projection a Linux desktop / OEM shell controller would consume.
    {
      auto& bridge = hosts::media::MediaBridge::instance();
      hosts::media::NowPlaying np;
      np.title = title;
      np.artist = artist;
      np.state = hosts::media::playback_from_state(state);
      bridge.publish(std::move(np));
      const auto m = bridge.mpris();
      log::i(kTag, "MediaBridge: published now-playing gen={} MPRIS PlaybackStatus={} title='{}'",
             bridge.generation(), m.playback_status, m.title);
    }
#endif
    if (session != nullptr) invoke_transport(ses_cls, session);
    if (ses_cls != nullptr) env_->DeleteLocalRef(ses_cls);
    if (session != nullptr) env_->DeleteLocalRef(session);
    env_->DeleteLocalRef(token_cls);
    env_->DeleteLocalRef(token);
    return Error::ok();
  }

  // Invoke one MediaSession.Callback transport method by name (no-op if absent).
  void dispatch_callback(jobject cb, jclass cb_cls, const char* method) {
    jmethodID m = env_->GetMethodID(cb_cls, method, "()V");
    if (m == nullptr) { clear_exception(); return; }
    env_->CallVoidMethod(cb, m);
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();
      env_->ExceptionClear();
    }
  }

  // Drive transport controls in-process (no controller/Binder). Under the automotive
  // profile a controller (MPRIS / OEM shell) posts commands into the media bridge;
  // we drain that channel and dispatch each to the app's MediaSession.Callback —
  // the app's overrides react. Otherwise a fixed Play/Pause pair is invoked directly.
  void invoke_transport(jclass ses_cls, jobject session) {
    jobject cb = env_->CallObjectMethod(
        session, env_->GetMethodID(ses_cls, "aclCallback",
                                   "()Landroid/media/session/MediaSession$Callback;"));
    if (cb == nullptr) { clear_exception(); return; }  // app set no callback
    jclass cb_cls = env_->GetObjectClass(cb);
#if ACL_PROFILE_AUTOMOTIVE
    auto& bridge = hosts::media::MediaBridge::instance();
    bridge.post_command(hosts::media::Command::Play);   // as an MPRIS controller would
    bridge.post_command(hosts::media::Command::Pause);
    hosts::media::Command c;
    while (bridge.next_command(c)) {
      const char* method = hosts::media::command_callback(c);
      log::i(kTag, "MediaBridge: dispatching command {} -> {}",
             hosts::media::command_name(c), method);
      dispatch_callback(cb, cb_cls, method);
      if (c == hosts::media::Command::Play) {
        // Playback started: the bridge routes the now-playing audio to the device.
        const std::uint64_t frames = bridge.route_now_playing();
        log::i(kTag, "MediaBridge: routing now-playing '{}' to PipeWire ({} frames)",
               bridge.snapshot().title, frames);
      }
    }
#else
    for (const char* action : {"onPlay", "onPause"}) dispatch_callback(cb, cb_cls, action);
#endif
    env_->DeleteLocalRef(cb_cls);
    env_->DeleteLocalRef(cb);
  }

  // Invoke one lifecycle callback (found on `cls` or a superclass). `pass_bundle`
  // passes a null Bundle (a fresh launch); absence of the method is not an error
  // (a subclass may not override every callback beyond the ones Activity defines).
  Error call_lifecycle(jclass cls, jobject activity, const char* method,
                       const char* sig, bool pass_bundle) {
    jmethodID mid = env_->GetMethodID(cls, method, sig);
    if (mid == nullptr) {  // shouldn't happen (Activity defines them), but be safe
      clear_exception();
      return Error::ok();
    }
    if (pass_bundle) {
      env_->CallVoidMethod(activity, mid, nullptr);  // savedInstanceState = null
    } else {
      env_->CallVoidMethod(activity, mid);
    }
    if (env_->ExceptionCheck() == JNI_TRUE) {
      return fail_with_exception(std::string(method) + "()");
    }
    return Error::ok();
  }

  jobjectArray make_string_array(const std::vector<std::string>& args) {
    jclass str_cls = env_->FindClass("java/lang/String");
    if (str_cls == nullptr) {
      clear_exception();
      return nullptr;
    }
    jobjectArray arr =
        env_->NewObjectArray(static_cast<jsize>(args.size()), str_cls, nullptr);
    if (arr != nullptr) {
      for (std::size_t i = 0; i < args.size(); ++i) {
        jstring s = env_->NewStringUTF(args[i].c_str());
        env_->SetObjectArrayElement(arr, static_cast<jsize>(i), s);
        env_->DeleteLocalRef(s);
      }
    }
    env_->DeleteLocalRef(str_cls);
    return arr;
  }

 protected:
  VmConfig config_;
  JavaVM* jvm_ = nullptr;
  JNIEnv* env_ = nullptr;
#if ACL_PROFILE_AUTOMOTIVE
  ToneAudioRoute audio_route_;  // media bridge -> acl::audio -> PipeWire
#endif
};

// OpenJDK backend (ADR-0001 fallback): links HotSpot's JNI_CreateJavaVM. A stock
// JVM has no separately repopulatable boot classpath, so the vendored framework
// (boot_classpath) is folded into java.class.path ahead of the app classes. Stock
// JVM bytecode only — DEX is fed via the dex_to_jvm translator (--convert-dex).
class JvmVm final : public JniVm {
 public:
  explicit JvmVm(VmConfig config) : JniVm(Backend::Jvm, std::move(config)) {}

 protected:
  Error create_java_vm(JNIEnv*& env) override {
    std::vector<std::string> cp = config_.boot_classpath;
    cp.insert(cp.end(), config_.classpath.begin(), config_.classpath.end());

    std::vector<std::string> option_strings;
    option_strings.push_back("-Djava.class.path=" + join(cp, ':'));
    for (const std::string& opt : config_.options) option_strings.push_back(opt);

    std::vector<JavaVMOption> options;
    options.reserve(option_strings.size());
    for (std::string& s : option_strings) {
      options.push_back(JavaVMOption{s.data(), nullptr});
    }

    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_8;
    args.nOptions = static_cast<jint>(options.size());
    args.options = options.data();
    args.ignoreUnrecognized = JNI_FALSE;

    // dlopen the host libjvm.so with RTLD_LOCAL rather than link it (DT_NEEDED). A direct
    // link loads HotSpot into the global symbol scope on EVERY run, so a subsequent --vm=art
    // run's AOSP libopenjdk.so binds JVM_* (e.g. Runtime.maxMemory -> JVM_MaxMemory) to the
    // host HotSpot instead of AOSP's libopenjdkjvm.so and crashes. RTLD_LOCAL keeps HotSpot's
    // symbols private to this backend, which only runs when --vm=jvm is explicitly selected.
    using CreateVmFn = jint (*)(JavaVM**, void**, void*);
    const char* libjvm = ACL_JVM_LIBJVM;
    if (libjvm[0] == '\0') {
      return Error::of(Status::InvalidConfig,
                       "no host libjvm.so detected at build time (--vm=jvm unavailable)");
    }
    void* handle = ::dlopen(libjvm, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      const char* e = ::dlerror();
      return Error::of(Status::RuntimeError,
                       std::string("dlopen(") + libjvm + ") failed: " + (e ? e : "?"));
    }
    auto create = reinterpret_cast<CreateVmFn>(::dlsym(handle, "JNI_CreateJavaVM"));
    if (create == nullptr) {
      return Error::of(Status::RuntimeError,
                       std::string("JNI_CreateJavaVM not found in ") + libjvm);
    }

    const jint rc = create(&jvm_, reinterpret_cast<void**>(&env), &args);
    if (rc != JNI_OK || jvm_ == nullptr) {
      jvm_ = nullptr;
      return Error::of(Status::RuntimeError,
                       "JNI_CreateJavaVM failed (rc=" + std::to_string(rc) + ")");
    }
    return Error::ok();
  }
};

// Host ART backend (ADR-0001 target): dlopens a prebuilt host ART's libart.so and
// boots it via the JNI Invocation API, so the app's DEX runs natively — no
// translation. ACL_ART_HOME points at the host ART tree's out/host/linux-x86 (holds
// lib64/libart.so with RUNPATH=$ORIGIN so its deps resolve, plus apex/com.android.*
// with the core bootclasspath jars). M2 milestone: imageless + interpreter (-Xint);
// the dex2oat'd framework boot image and app OAT cache are M3.
class ArtVm final : public JniVm {
 public:
  explicit ArtVm(VmConfig config) : JniVm(Backend::Art, std::move(config)) {}

 protected:
  Error create_java_vm(JNIEnv*& env) override {
    const char* home_c = std::getenv("ACL_ART_HOME");
    if (home_c == nullptr || *home_c == '\0') {
      return Error::of(
          Status::InvalidConfig,
          "ACL_ART_HOME not set: point it at a host ART build's out/host/linux-x86 "
          "(lib64/libart.so + apex/com.android.art). See docs/art-host-spike.md");
    }
    const std::string home = home_c;

    // Host ART locates its runtime/data modules via these roots. Note the host
    // apex layout is mixed: the javalib bootclasspath jars + boot image live under
    // $HOME/apex/com.android.*/, but the ICU/tzdata *data* roots are the non-apex
    // $HOME/com.android.{i18n,tzdata} (that is where etc/icu/icudt*.dat lives, and
    // libicu_jni aborts at startup if ANDROID_I18N_ROOT does not point at it).
    ::setenv("ANDROID_ROOT", home.c_str(), 1);
    ::setenv("ANDROID_ART_ROOT", (home + "/apex/com.android.art").c_str(), 1);
    ::setenv("ANDROID_I18N_ROOT", (home + "/com.android.i18n").c_str(), 1);
    ::setenv("ANDROID_TZDATA_ROOT", (home + "/com.android.tzdata").c_str(), 1);
    if (std::getenv("ANDROID_DATA") == nullptr) {
      ::setenv("ANDROID_DATA", (home + "/acl-android-data").c_str(), 1);
    }

    // libart.so carries RUNPATH=$ORIGIN, so its lib64/ siblings resolve without
    // LD_LIBRARY_PATH; RTLD_GLOBAL exposes ART's JNI symbols to loaded natives.
    const std::string libart = home + "/lib64/libart.so";
    void* handle = ::dlopen(libart.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
      const char* e = ::dlerror();
      return Error::of(Status::Unavailable,
                       "dlopen(" + libart + ") failed: " + (e ? e : "?"));
    }
    using CreateVmFn = jint (*)(JavaVM**, void**, void*);
    auto create_vm =
        reinterpret_cast<CreateVmFn>(::dlsym(handle, "JNI_CreateJavaVM"));
    if (create_vm == nullptr) {
      return Error::of(Status::Unavailable,
                       "JNI_CreateJavaVM not found in " + libart);
    }

    // Core boot classpath (java.* + a few libs), in ART image order. ART loads
    // these DEX jars natively. config_.boot_classpath (acl's android.jar stubs, for
    // the JVM backend) does not apply here — the real framework is an M3 boot image.
    struct BcpJar { const char* module; const char* apex; };
    static const BcpJar kBcp[] = {
        {"core-oj", "com.android.art"},     {"core-libart", "com.android.art"},
        {"okhttp", "com.android.art"},      {"bouncycastle", "com.android.art"},
        {"apache-xml", "com.android.art"},  {"core-icu4j", "com.android.i18n"},
        {"conscrypt", "com.android.conscrypt"}};
    std::vector<std::string> bcp;
    for (const BcpJar& j : kBcp) {
      bcp.push_back(home + "/apex/" + j.apex + "/javalib/" + j.module + ".jar");
    }
    // P2.1 (ADR-0002): append the real framework to the BOOT classpath (not the app
    // classpath) so framework classes get boot-classloader identity as on device —
    // needed for framework internals that assume bootclasspath residency. Colon-
    // separated jars (framework-minus-apex, framework-graphics, ext, ...).
    if (const char* ext = std::getenv("ACL_ART_BOOTCLASSPATH");
        ext != nullptr && *ext != '\0') {
      bcp.emplace_back(ext);
    }
    const std::string bcp_joined = join(bcp, ':');

    // Application classpath: the vendored android.* framework DEX (ADR-0002) ahead
    // of the app, so real framework classes (android.app.Activity, ...) resolve —
    // the JVM backend folds these into java.class.path from .class dirs, but ART
    // needs DEX, so ACL_ART_FRAMEWORK carries the d8'd framework (colon-separated;
    // shim dex before the android.jar-stub dex). Absent it, only java.* is available
    // and apps that touch the framework fail to load — a static-main DEX still runs.
    std::vector<std::string> cp;
    if (const char* fw = std::getenv("ACL_ART_FRAMEWORK"); fw != nullptr && *fw != '\0') {
      cp.emplace_back(fw);
    } else if (std::getenv("ACL_ART_BOOTCLASSPATH") == nullptr) {
      // Say so here, plainly. Without a framework ART can read, android.* does not resolve, and
      // the first thing that fails is the APP's own Activity — "could not load class
      // com.example.MainActivity: ClassNotFoundException" — which points at the app rather than at
      // the missing framework, and costs an hour to see through. acl's staged framework
      // (config_.boot_classpath) is .class files + android.jar: that is what the JVM backend eats,
      // but ART reads only DEX, so it is not usable here and is NOT what ART is booting with.
      log::w(kTag,
             "no framework for ART: neither ACL_ART_FRAMEWORK (d8'd android.* DEX) nor "
             "ACL_ART_BOOTCLASSPATH (real framework jars) is set. acl's staged framework{} is "
             ".class/.jar, which ART cannot read. android.* will not resolve, so any app with an "
             "Activity fails to load its own class; a static-main DEX still runs. See "
             "docs/ndk-scope.md and tests/integration/run_activity_art_test.py for the d8 recipe.",
             config_.boot_classpath.empty() ? "" : (" (" + join(config_.boot_classpath, ':') + ")"));
    }
    // acl's host-ART runtime bootstrap DEX (org.acl.internal.HostBootstrap): the framework
    // environment (main Looper, no-Binder ServiceManager, minimal Context) an app expects
    // the runtime to provide. Injected ahead of the app so a pure app needs no setup.
    if (const char* hb = std::getenv("ACL_ART_HOST_BOOTSTRAP"); hb != nullptr && *hb != '\0') {
      cp.emplace_back(hb);
    }
    cp.insert(cp.end(), config_.classpath.begin(), config_.classpath.end());

    std::vector<std::string> option_strings;
    option_strings.push_back("-Xbootclasspath:" + bcp_joined);
    option_strings.push_back("-Xbootclasspath-locations:" + bcp_joined);
    // P2.1: a dex2oat'd framework boot image (ACL_ART_BOOT_IMAGE) makes the framework
    // AOT-compiled on the bootclasspath; without one, interpret imageless (-Xint). The
    // image's baked bootclasspath must match ACL_ART_BOOTCLASSPATH or ART falls back.
    if (const char* img = std::getenv("ACL_ART_BOOT_IMAGE");
        img != nullptr && *img != '\0') {
      option_strings.push_back(std::string("-Ximage:") + img);
    } else if (const char* jit = std::getenv("ACL_ART_JIT"); jit != nullptr && *jit != '\0') {
      // Imageless but JIT-compiled. -Xint is stronger than the imageless constraint requires:
      // the JIT compiles hot methods in memory at runtime and needs no boot image (that is
      // dex2oat's job, not the JIT's). Interpreter-only costs roughly an order of magnitude,
      // which a heavy app (React Native class loading, Compose) shows as an unresponsive UI.
      option_strings.push_back("-Xusejit:true");
    } else {
      option_strings.push_back("-Xint");  // imageless: interpret; no runtime dex2oat
    }
    option_strings.push_back("-Djava.class.path=" + join(cp, ':'));
    // Where System.loadLibrary resolves boot-classpath native libs (acl's libsoundpool.so — the
    // android.media.SoundPool <clinit> loadLibrary("soundpool") — and any future supplemental .so).
    if (const char* libpath = std::getenv("ACL_ART_LIBPATH"); libpath != nullptr && *libpath != '\0') {
      option_strings.push_back(std::string("-Djava.library.path=") + libpath);
    }
    // Extra raw ART runtime options, space-separated (e.g. "-verbose:class -verbose:jni" to
    // surface the missing class behind an app-swallowed NoClassDefFoundError). Debug/diagnostic.
    if (const char* opts = std::getenv("ACL_ART_OPTS"); opts != nullptr && *opts != '\0') {
      std::string cur;
      for (const char* c = opts;; ++c) {
        if (*c == ' ' || *c == '\0') {
          if (!cur.empty()) { option_strings.push_back(cur); cur.clear(); }
          if (*c == '\0') break;
        } else {
          cur.push_back(*c);
        }
      }
    }
    for (const std::string& opt : config_.options) option_strings.push_back(opt);
    // JVMTI exception-naming diagnostic (ACL_JVMTI_DIAG): load the JVMTI plugin so GetEnv() can
    // attach an Exception hook that names boot-classloader NoClassDefFoundErrors.
    // ACL_STALL_DIAG (the Java-stack sampler) needs the same plugin.
    const bool jvmti_diag = [] {
      for (const char* v : {std::getenv("ACL_JVMTI_DIAG"), std::getenv("ACL_STALL_DIAG")})
        if (v != nullptr && *v != '\0') return true;
      return false;
    }();
    if (jvmti_diag) {
      option_strings.push_back("-Xplugin:libopenjdkjvmti.so");  // non-debug — must match libartbase.so
      option_strings.push_back("-XjdwpProvider:none");
    }

    std::vector<JavaVMOption> options;
    options.reserve(option_strings.size());
    for (std::string& s : option_strings) {
      options.push_back(JavaVMOption{s.data(), nullptr});
    }

    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_6;
    args.nOptions = static_cast<jint>(options.size());
    args.options = options.data();
    args.ignoreUnrecognized = JNI_FALSE;

    const jint rc = create_vm(&jvm_, reinterpret_cast<void**>(&env), &args);
    if (rc != JNI_OK || jvm_ == nullptr) {
      jvm_ = nullptr;
      return Error::of(Status::RuntimeError,
                       "ART JNI_CreateJavaVM failed (rc=" + std::to_string(rc) +
                           "); ACL_ART_HOME=" + home);
    }
    if (jvmti_diag) install_jvmti_diag(jvm_);
    return Error::ok();
  }

  // ADR-0008: register the real framework's native methods from the host
  // libandroid_runtime so android.* natives (SystemProperties, Parcel, resources, …)
  // execute. Opt-in via ACL_ART_NATIVE, and only meaningful with the real framework
  // (framework-minus-apex) on the classpath so each register_*'s FindClass resolves.
  // Without it, fall back to acl's shim natives (the M2/M3 path). The host lib strips
  // AndroidRuntime::startReg, but the per-component register_android_*(env) functions
  // are exported, so acl calls a curated set of the pure-framework ones directly (no
  // AndroidRuntime/gCurRuntime dependency).
  void register_natives(JNIEnv* env) override {
    // Back libandroid's NDK AAssetManager with the app APK, so an app's native code (e.g. Flutter's
    // engine loading its Dart kernel + flutter_assets) can read assets. The APK is the app classpath
    // entry; the provider opens it lazily. Independent of the native floor below.
    if (!config_.classpath.empty()) {
      g_asset_apk_path = config_.classpath.front();
      acl::ndk::set_asset_provider(&acl_pkg_asset_open, &acl_pkg_asset_free, nullptr);
    }
    // The app's own bionic lib/<abi>/*.so loading (ADR-5) is orthogonal to the framework
    // native floor below — an app ships native code whether or not the real
    // libandroid_runtime is in play — so arm it first, ahead of the floor's early returns.
    register_ndk_loadlibrary_override(env);  // opt-in (ACL_BIONIC_LINKER); no-op otherwise
    const char* opt = std::getenv("ACL_ART_NATIVE");
    const char* home_c = std::getenv("ACL_ART_HOME");
    if (opt == nullptr || *opt == '\0' || home_c == nullptr || *home_c == '\0') {
      register_framework_natives(env);  // shim floor (M2/M3)
      return;
    }
    const std::string lib = std::string(home_c) + "/lib64/libandroid_runtime.so";
    void* h = ::dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
      const char* e = ::dlerror();
      log::w(kTag, "ART native floor: dlopen({}) failed: {}; using shim natives", lib,
             e ? e : "?");
      register_framework_natives(env);  // fall back to the shim floor
      return;
    }
    // The framework native registrars the host libandroid_runtime exports (its
    // LayoutlibLoader host build compiles these device-independent-enough natives —
    // resources, input, os, db, animation). Each register_*(env) FindClass'es its
    // framework class and RegisterNatives; missing ones are cleared and counted. This
    // clears the real-Activity frontier (Resources -> AssetManager native). Natives
    // NOT here (View, Bitmap/Canvas, Configuration) stay target-only — the P2.4 render
    // subsystem (docs/resources-host-scope.md, ADR-0008).
    static constexpr const char* kReg[] = {
        // resources
        "_ZN7android37register_android_content_AssetManagerEP7_JNIEnv",
        "_ZN7android38register_android_content_res_ApkAssetsEP7_JNIEnv",
        "_ZN7android33register_android_content_XmlBlockEP7_JNIEnv",
        "_ZN7android36register_android_content_StringBlockEP7_JNIEnv",
        // os / looper / props
        "_ZN7android36register_android_os_SystemPropertiesEP7_JNIEnv",
        "_ZN7android31register_android_os_SystemClockEP7_JNIEnv",
        "_ZN7android25register_android_os_TraceEP7_JNIEnv",
        "_ZN7android26register_android_os_ParcelEP7_JNIEnv",
        "_ZN7android32register_android_os_MessageQueueEP7_JNIEnv",
        "_ZN7android32register_android_os_FileObserverEP7_JNIEnv",
        // util
        "_ZN7android25register_android_util_LogEP7_JNIEnv",
        "_ZN7android30register_android_util_EventLogEP7_JNIEnv",
        "_ZN7android38register_android_text_AndroidCharacterEP7_JNIEnv",
        "_ZN7android39register_android_util_jar_StrictJarFileEP7_JNIEnv",
        // input / view (event plumbing; not the View/graphics render natives)
        "_ZN7android30register_android_view_KeyEventEP7_JNIEnv",
        "_ZN7android33register_android_view_MotionEventEP7_JNIEnv",
        "_ZN7android33register_android_view_InputDeviceEP7_JNIEnv",
        "_ZN7android34register_android_view_InputChannelEP7_JNIEnv",
        "_ZN7android37register_android_view_KeyCharacterMapEP7_JNIEnv",
        "_ZN7android37register_android_view_VelocityTrackerEP7_JNIEnv",
        "_ZN7android38register_android_view_InputEventSenderEP7_JNIEnv",
        "_ZN7android40register_android_view_InputEventReceiverEP7_JNIEnv",
        "_ZN7android44register_android_view_InputApplicationHandleEP7_JNIEnv",
        // database / animation
        "_ZN7android38register_android_database_CursorWindowEP7_JNIEnv",
        "_ZN7android38register_android_database_SQLiteGlobalEP7_JNIEnv",
        "_ZN7android37register_android_database_SQLiteDebugEP7_JNIEnv",
        "_ZN7android42register_android_database_SQLiteConnectionEP7_JNIEnv",
        "_ZN7android47register_android_animation_PropertyValuesHolderEP7_JNIEnv",
    };
    using RegFn = jint (*)(JNIEnv*);
    int ok = 0, unavailable = 0;
    for (const char* sym : kReg) {
      auto fn = reinterpret_cast<RegFn>(::dlsym(h, sym));
      if (fn == nullptr) { ++unavailable; continue; }
      const jint rc = fn(env);
      if (env->ExceptionCheck()) { env->ExceptionClear(); ++unavailable; continue; }
      if (rc == JNI_OK) ++ok; else ++unavailable;
    }
    log::i(kTag,
           "ART native floor: {} framework native group(s) from libandroid_runtime "
           "({} unavailable)",
           ok, unavailable);
    // libandroid_runtime's host build omits graphics-buffer natives, so supply acl's (the vendored
    // android.hardware.HardwareBuffer's native underside over a gbm dma-buf — Flutter external textures).
    register_acl_supplemental_natives(env);
    // Override InputEventReceiver.nativeDispose. The real one (just registered above) destructs
    // NativeInputEventReceiver, whose destructor calls AndroidRuntime::getJNIEnv() — null on acl (we
    // run the JNI Invocation API directly, never AndroidRuntime::startVm, so its mJavaVM is null) →
    // SIGSEGV on any window/overlay teardown (dismissing a Dialog/Popup/menu). No-op it: the small
    // native receiver + its InputChannel leak per torn-down window instead of crashing the process.
    if (jclass ier = env->FindClass("android/view/InputEventReceiver")) {
      const JNINativeMethod m = {"nativeDispose", "(J)V",
                                 reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})};
      if (env->RegisterNatives(ier, &m, 1) == JNI_OK)
        log::i(kTag, "InputEventReceiver.nativeDispose -> acl no-op (avoids AndroidRuntime::getJNIEnv)");
      env->DeleteLocalRef(ier);
    }
    // android.media.AudioSystem.<clinit> seeds OUT_CHANNEL_COUNT_MAX / SAMPLE_RATE_HZ_{MAX,MIN}
    // from three native getters. The host libandroid_runtime (no audio HAL) does not register
    // AudioSystem, so its class init fails with UnsatisfiedLinkError and takes AudioFormat/AudioTrack
    // down with it (agrigolo.chubbyclick's metronome). Bind the three getters to the AOSP constants
    // (FCC_LIMIT + the sample-rate bounds). ART's FindClass loads without running <clinit>, so this
    // binds before the app first touches the class.
    if (jclass as_cls = env->FindClass("android/media/AudioSystem")) {
      const JNINativeMethod m[] = {
          {const_cast<char*>("native_getMaxChannelCount"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return 24; })},
          {const_cast<char*>("native_getMaxSampleRate"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return 192000; })},
          {const_cast<char*>("native_getMinSampleRate"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return 4000; })},
          // ExoPlayer's constructor allocates an audio session id (Util.generateAudioSessionIdV21 ->
          // AudioManager.generateAudioSessionId -> AudioSystem.newAudioSessionId). Unbound, the whole
          // ExoPlayer:Playback thread dies and video_player's initialize() hangs. Hand out a unique id.
          {const_cast<char*>("newAudioSessionId"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint {
             static std::atomic<jint> next{10000};
             return next.fetch_add(1, std::memory_order_relaxed);
           })},
      };
      if (env->RegisterNatives(as_cls, m, 4) == JNI_OK)
        log::i(kTag, "android.media.AudioSystem <clinit> getters bound (channel/sample-rate maxima, session id)");
      env->DeleteLocalRef(as_cls);
    }
    // androidx.media3 / MediaSessionCompat build AudioAttributes from a legacy stream type
    // (setPlaybackToLocal -> AudioAttributes.Builder.setLegacyStreamType), which consults
    // AudioProductStrategy.getAudioProductStrategies(). That native lists the vendor audio-policy
    // engine's product strategies; the host libandroid_runtime (no audio HAL / policy config) does
    // not register it, so the UnsatisfiedLinkError takes the media session down with it. acl has no
    // audio policy engine, so the truthful answer is "no strategies": report AudioSystem.SUCCESS and
    // leave the caller's ArrayList empty. AudioAttributes then falls through to the platform's own
    // legacy stream -> content-type mapping (its strategy branch is guarded by size() > 0).
    if (jclass aps_cls = find_class_optional(env, "android/media/audiopolicy/AudioProductStrategy")) {
      const JNINativeMethod m = {
          const_cast<char*>("native_list_audio_product_strategies"),
          const_cast<char*>("(Ljava/util/ArrayList;)I"),
          reinterpret_cast<void*>(+[](JNIEnv*, jclass, jobject) -> jint { return 0; })};
      if (env->RegisterNatives(aps_cls, &m, 1) == JNI_OK)
        log::i(kTag, "AudioProductStrategy.native_list_audio_product_strategies -> acl empty "
                     "(no audio policy engine; legacy stream mapping applies)");
      env->DeleteLocalRef(aps_cls);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    // ExoPlayer/video_player call Surface.isValid()/release() on the output Surface (acl's ImageReader
    // producer Surface, mNativeObject = ReaderCtx). Surface(long) is pure-Java on acl, so these natives
    // are otherwise absent; an unbound nativeIsValid kills the ExoPlayer:Playback thread. A non-null
    // handle is valid; release is a no-op (the ImageReader owns the buffers).
    if (jclass sf = find_class_optional(env, "android/view/Surface")) {
      const JNINativeMethod m[] = {
          {const_cast<char*>("nativeIsValid"), const_cast<char*>("(J)Z"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong p) -> jboolean { return p != 0 ? JNI_TRUE : JNI_FALSE; })},
          {const_cast<char*>("nativeRelease"), const_cast<char*>("(J)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})},
          // ExoPlayer's VideoFrameReleaseHelper hints the display frame rate; acl presents on
          // wp_presentation, so accept + ignore it (0 = NO_ERROR).
          {const_cast<char*>("nativeSetFrameRate"), const_cast<char*>("(JFII)I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jfloat, jint, jint) -> jint { return 0; })},
      };
      if (env->RegisterNatives(sf, m, 3) == JNI_OK)
        log::i(kTag, "android.view.Surface.nativeIsValid/nativeRelease/nativeSetFrameRate bound");
      env->DeleteLocalRef(sf);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    // android.graphics.HardwareRendererObserver — Chromium WebView's AwContents attaches a frame-metrics
    // observer (FrameMetrics), whose natives live in libhwui (dropped by acl). Unbound, nCreateObserver
    // throws UnsatisfiedLinkError and AwContents.<init> fails -> WebView inflation fails. acl has no hwui
    // frame timeline: hand back an observer handle and report "no frame data" (nGetNextBuffer=-1).
    //
    // The handle MUST be a real object, not a dummy sentinel: HardwareRendererObserver.<init> wraps it in
    // com.android.internal.util.VirtualRefBasePtr, which immediately nIncStrong()s it (and nDecStrong()s on
    // release). graphics_jni's VirtualRefBasePtr natives reinterpret_cast the handle to a VirtualLightRefBase
    // (vptr@0, atomic<int32_t> refcount@8) and inc/decStrong it, deleting at zero — so a sentinel like 1
    // dereferences ~0x9 and SIGSEGVs (which WebView's crashpad then turns into a fatal _exit). Return an
    // ABI-identical VirtualLightRefBase so the inc/decStrong round-trips and frees cleanly on GC.
    if (jclass hro = find_class_optional(env, "android/graphics/HardwareRendererObserver")) {
      struct AclVirtualLightRef {  // layout-compatible with graphics_jni's VirtualLightRefBase
        virtual ~AclVirtualLightRef() = default;
        mutable std::atomic<std::int32_t> ref_count{0};
      };
      const JNINativeMethod m[] = {
          {const_cast<char*>("nCreateObserver"), const_cast<char*>("(Ljava/lang/ref/WeakReference;Z)J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jobject, jboolean) -> jlong {
             return reinterpret_cast<jlong>(new AclVirtualLightRef());
           })},
          {const_cast<char*>("nGetNextBuffer"), const_cast<char*>("(J[J)I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlongArray) -> jint { return -1; })},
      };
      if (env->RegisterNatives(hro, m, 2) == JNI_OK)
        log::i(kTag, "android.graphics.HardwareRendererObserver stubbed (no hwui frame metrics)");
      env->DeleteLocalRef(hro);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    // android.graphics.HardwareRenderer.nAddObserver/nRemoveObserver — View.addFrameMetricsListener
    // (reached via WebView's WindowAndroid during dispatchAttachedToWindow) registers a frame-metrics
    // observer with the render proxy. libhwui is dropped, so these are unbound and the
    // UnsatisfiedLinkError aborts dispatchAttachedToWindow — leaving the WHOLE view tree (incl. the
    // WebView) not-attached-to-window, which forces WebView into the software draw path (its
    // OnDrawHardware gate is `is_hardware_accelerated && attached_to_window()`). No-op them: acl has no
    // hwui frame timeline to observe, and the observer object is inert.
    if (jclass hr = find_class_optional(env, "android/graphics/HardwareRenderer")) {
      const JNINativeMethod m[] = {
          {const_cast<char*>("nAddObserver"), const_cast<char*>("(JJ)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong) {})},
          {const_cast<char*>("nRemoveObserver"), const_cast<char*>("(JJ)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong) {})},
      };
      if (env->RegisterNatives(hr, m, 2) == JNI_OK)
        log::i(kTag, "android.graphics.HardwareRenderer.nAddObserver/nRemoveObserver stubbed");
      env->DeleteLocalRef(hr);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    // android.media.AudioPortEventHandler.native_setup — audio_service (com.ryanheise.audioservice)
    // registers an AudioDeviceCallback during Flutter plugin registration, which inits an
    // AudioPortEventHandler whose native_setup the host libandroid_runtime (no audio HAL) doesn't
    // provide. The UnsatisfiedLinkError makes the WHOLE GeneratedPluginRegistrant throw (Flutter
    // catches it), so audio_service never registers its method channel — and the app's Dart main()
    // then awaits AudioService.init() forever, never reaching runApp(), so no frame is ever scheduled
    // (measured: FlutterJNI.asyncWaitForVsync is never called). No-op it so registration completes.
    if (jclass ape = find_class_optional(env, "android/media/AudioPortEventHandler")) {
      const JNINativeMethod m = {const_cast<char*>("native_setup"),
                                 const_cast<char*>("(Ljava/lang/Object;)V"),
                                 reinterpret_cast<void*>(+[](JNIEnv*, jobject, jobject) {})};
      if (env->RegisterNatives(ape, &m, 1) == JNI_OK)
        log::i(kTag, "android.media.AudioPortEventHandler.native_setup -> acl no-op (audio_service)");
      env->DeleteLocalRef(ape);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    // AudioManager.registerAudioDeviceCallback (next in the same audio_service chain) enumerates
    // audio ports via AudioSystem.listAudioPorts/listAudioPatches — device-only natives. Report "no
    // ports" success with matching port/patch generations so updateAudioPortCache's retry loop exits.
    if (jclass as = find_class_optional(env, "android/media/AudioSystem")) {
      const auto empty_ports = +[](JNIEnv* e, jclass, jobject, jintArray gen) -> jint {
        if (gen != nullptr && e->GetArrayLength(gen) >= 1) { jint one = 1; e->SetIntArrayRegion(gen, 0, 1, &one); }
        return 0;  // AudioManager.SUCCESS
      };
      const JNINativeMethod m[] = {
          {const_cast<char*>("listAudioPorts"), const_cast<char*>("(Ljava/util/ArrayList;[I)I"),
           reinterpret_cast<void*>(empty_ports)},
          {const_cast<char*>("listAudioPatches"), const_cast<char*>("(Ljava/util/ArrayList;[I)I"),
           reinterpret_cast<void*>(empty_ports)},
      };
      if (env->RegisterNatives(as, m, 2) == JNI_OK)
        log::i(kTag, "android.media.AudioSystem.listAudioPorts/Patches -> acl no-op success");
      env->DeleteLocalRef(as);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    populate_system_properties(env);
    load_system_resources(env);
    register_no_binder(env);
    register_systemproperties_override(env);  // neutralize the device-only fast-property abort
    bootstrap_host_env(env);
    load_acl_graphics(env);
    load_acl_gles(env);   // android.opengl.GLES20/GLES30 natives (GLSurfaceView path)
    load_acl_egl10(env);  // com.google.android.gles_jni.EGLImpl natives (GLSurfaceView path)
    // Seed default typefaces now that the graphics Font/Typeface natives are registered
    // (the system font map isn't pushed to a standalone run).
    if (jclass boot = find_class_optional(env, "org/acl/internal/HostBootstrap"); boot != nullptr) {
      if (jmethodID m = env->GetStaticMethodID(boot, "seedTypefaces", "()V"))
        env->CallStaticVoidMethod(boot, m);
      if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
      env->DeleteLocalRef(boot);
    }
#if ACL_HAVE_SDBUS
    // Bind android.net.ConnectivityManager (acl clean-room shim on the boot classpath)
    // to the svc/net host connectivity backend (systemd-networkd/NetworkManager/ConnMan).
    acl::net::register_connectivity_natives(env);
    acl::net::register_wifi_natives(env);
    // Bind org.acl.internal.NotifyBridge (INotificationManager translation) to the
    // svc/notify host backend (org.freedesktop.Notifications).
    acl::notify::register_notify_natives(env);
    // Bind org.acl.internal.BatteryBridge (ACTION_BATTERY_CHANGED extras) to the svc/battery
    // host power backend (org.freedesktop.UPower).
    acl::battery::register_battery_natives(env);
    acl::power::register_power_natives(env);
    acl::nsd::register_nsd_natives(env);
    // Bind org.acl.internal.BluetoothBridge (android.bluetooth.* shims) to the svc/bluetooth
    // host BlueZ backend (org.bluez).
    acl::bt::register_bt_natives(env);
    // Bind org.acl.internal.StorageBridge (StorageManager/IStorageManager.getVolumeList) to the
    // svc/storage host UDisks2 backend (removable media).
    acl::storage::register_storage_natives(env);
#endif
    // Bind org.acl.internal.AclSensorManager to the svc/sensor hub (Linux IIO / synthetic). No
    // D-Bus dependency, so this is outside the sdbus block.
    acl::svc::sensor::register_sensor_natives(env);
    // Bind org.acl.internal.AclLocationManager to the svc/location hub (host GeoClue2 over D-Bus
    // when built with sdbus-c++, else a synthetic fixed point). The GeoClue2 source is gated inside
    // the module, so registration itself is unconditional (the synthetic/null source always builds).
    acl::svc::location::register_location_natives(env);
#if ACL_HAVE_PIPEWIRE
    // Bind org.acl.internal.AudioBridge (AudioManager master volume) to the host PipeWire
    // default sink.
    acl::audio::register_audio_natives(env);
#endif
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    // Bind org.acl.internal.PresentDriver's native seam (the Looper-driven present, approach A).
    register_present_driver_natives(env);
#endif
  }

  // Run acl's host-ART runtime bootstrap (org.acl.internal.HostBootstrap.init) if injected on
  // the classpath: prepare the main Looper (any Activity/Handler needs it) and seed a
  // no-Binder ServiceManager. Runs on EVERY host-ART boot, before app code, so the normal
  // launcher-Activity flow (not just the view-host demo) gets the environment.
  void bootstrap_host_env(JNIEnv* env) {
    if (jclass boot = find_class_optional(env, "org/acl/internal/HostBootstrap"); boot != nullptr) {
      if (jmethodID init = env->GetStaticMethodID(boot, "init", "()V"))
        env->CallStaticVoidMethod(boot, init);
      if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }
      env->DeleteLocalRef(boot);
      log::i(kTag, "host bootstrap: main Looper + no-Binder ServiceManager ready");
      return;
    }
    // No acl helper on the classpath means the REAL framework boot classpath (P2.2/ADR-0008),
    // where org.acl.internal.* legitimately does not exist. The main Looper still has to be
    // prepared — an Activity constructs Handlers, and without it the app dies on "Can't create
    // handler inside thread ... that has not called Looper.prepare()" before reaching onCreate.
    // The framework prepares it itself on device (ActivityThread.main); here acl is the one
    // standing in for ActivityThread, so call the framework's own entry point.
    jclass looper = find_class_optional(env, "android/os/Looper");
    if (looper == nullptr) return;  // a shim classpath without Looper: nothing to prepare
    jmethodID get_main = env->GetStaticMethodID(looper, "getMainLooper", "()Landroid/os/Looper;");
    jmethodID prepare_main = env->GetStaticMethodID(looper, "prepareMainLooper", "()V");
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    if (get_main != nullptr && prepare_main != nullptr) {
      // prepareMainLooper throws if one already exists, so ask first rather than lean on the throw.
      jobject existing = env->CallStaticObjectMethod(looper, get_main);
      if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
      if (existing == nullptr) {
        env->CallStaticVoidMethod(looper, prepare_main);
        if (env->ExceptionCheck() == JNI_TRUE) {
          env->ExceptionDescribe();
          env->ExceptionClear();
        } else {
          log::i(kTag, "host bootstrap: main Looper prepared (real framework classpath)");
        }
      } else {
        env->DeleteLocalRef(existing);
      }
    }
    env->DeleteLocalRef(looper);
  }

  // acl has no Binder (ADR-3): stub the ServiceManager entry point so the framework's
  // Binder path degrades gracefully instead of crashing with UnsatisfiedLinkError.
  // BinderInternal.getContextObject() → null makes ServiceManager.getService() return
  // null, and framework callers (ViewConfiguration, WindowManagerGlobal, …) fall back to
  // their no-service defaults. The Java-visible Manager APIs are served by svc/, not Binder.
  // android.os.SystemProperties.native_find (the fast-property-access path) LOG(FATAL)s on host
  // ("fast property access supported only on device", android_os_SystemProperties.cpp:143). Override
  // it to return 0 (prop not found) so callers (SystemProperties.find()) fall back to the slow
  // native_get(name) path, which works on host. Must run after the libandroid_runtime native floor
  // (which binds the aborting one). Hit by Flutter's FlutterLoader.startInitialization + ACRA.
  // Install the bionic NDK-loading path if configured (ACL_BIONIC_LINKER set). No-op otherwise, so
  // default runs keep ART's own nativeLoad. See ndk_native_load above + docs/ndk-scope.md.
  void register_ndk_loadlibrary_override(JNIEnv* env) {
#if ACL_HAVE_JNI
    const char* linker = std::getenv("ACL_BIONIC_LINKER");
    if (linker == nullptr || *linker == '\0') return;
    // Validate the config now (report a bad env at boot), but DON'T bootstrap the bionic linker
    // yet — bootstrapping is deferred to the first bionic .so (ensure_ndk_rt), so it happens after
    // load_system_resources rather than corrupting it. from_env() picks up ACL_BIONIC_APP_LIBS too.
    acl::ndk::BionicConfig cfg = acl::ndk::BionicConfig::from_env();
    std::string why;
    if (!cfg.valid(&why)) { log::w(kTag, "NDK bionic loader unavailable: {}", why); return; }
    g_ndk_cfg = cfg;
    g_ndk_have_cfg = true;
    env->GetJavaVM(&g_ndk_vm);
    if (jclass c = env->FindClass("java/lang/Runtime")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("nativeLoad"),
           const_cast<char*>(
               "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Class;)Ljava/lang/String;"),
           reinterpret_cast<void*>(&ndk_native_load)},
      };
      if (env->RegisterNatives(c, m, 1) != JNI_OK) env->ExceptionClear();
      else log::i(kTag, "NDK bionic loader armed (Runtime.nativeLoad routed; linker bootstraps lazily)");
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
#else
    (void)env;
#endif
  }

  void register_systemproperties_override(JNIEnv* env) {
    if (jclass c = env->FindClass("android/os/SystemProperties")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("native_find"), const_cast<char*>("(Ljava/lang/String;)J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jstring) -> jlong { return 0; })},
      };
      if (env->RegisterNatives(c, m, 1) != JNI_OK) env->ExceptionClear();
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
  }

  void register_no_binder(JNIEnv* env) {
    if (jclass c = env->FindClass("com/android/internal/os/BinderInternal")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("getContextObject"), const_cast<char*>("()Landroid/os/IBinder;"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jobject { return nullptr; })},
      };
      if (env->RegisterNatives(c, m, 1) != JNI_OK) env->ExceptionClear();
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
    // android.os.Binder identity statics: no Binder, so a single fixed in-process identity.
    if (jclass c = env->FindClass("android/os/Binder")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("getCallingUid"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return 1000; })},
          {const_cast<char*>("getCallingPid"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return 0; })},
          {const_cast<char*>("clearCallingIdentity"), const_cast<char*>("()J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong { return 0; })},
          {const_cast<char*>("restoreCallingIdentity"), const_cast<char*>("(J)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})},
          // Binder object construction (getNativeBBinderHolder) → a non-null sentinel (the
          // NativeAllocationRegistry rejects null). A Binder with no native backing: framework
          // client stubs (e.g. IAccessibilityManagerClient) construct fine; they only fail if
          // actually transacted, which has no service to reach anyway. The no-op finalizer
          // below never frees the shared sentinel.
          {const_cast<char*>("getNativeBBinderHolder"), const_cast<char*>("()J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong {
             static int holder = 0;
             return reinterpret_cast<jlong>(&holder);
           })},
          // The NativeAllocationRegistry finalizer for a Binder holder → a no-op free (the
          // holder is 0, so there is nothing to free).
          {const_cast<char*>("getNativeFinalizer"), const_cast<char*>("()J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong {
             return reinterpret_cast<jlong>(static_cast<void (*)(void*)>(+[](void*) {}));
           })},
          // Called from MessageQueue.next on background looper threads (no Binder to flush).
          {const_cast<char*>("flushPendingCommands"), const_cast<char*>("()V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) {})},
          // StrictMode thread policy rides in the Binder thread-local (device-side it maps to
          // the driver). No Binder → keep it in a process-local slot so set/get round-trips.
          {const_cast<char*>("setThreadStrictModePolicy"), const_cast<char*>("(I)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint policy) { g_strict_mode_policy = policy; })},
          {const_cast<char*>("getThreadStrictModePolicy"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return g_strict_mode_policy; })},
      };
      if (env->RegisterNatives(c, m, sizeof(m) / sizeof(m[0])) != JNI_OK) env->ExceptionClear();
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
    // android.os.Process thread ops touch device-only natives on background HandlerThreads
    // (e.g. SharedPreferences' queued-work looper). No-op them so those threads don't crash.
    if (jclass c = env->FindClass("android/os/Process")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("setThreadPriority"), const_cast<char*>("(I)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint) {})},
          {const_cast<char*>("setThreadPriority"), const_cast<char*>("(II)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint, jint) {})},
      };
      if (env->RegisterNatives(c, m, 2) != JNI_OK) env->ExceptionClear();
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
    log::i(kTag, "ART native floor: Binder/BinderInternal/Process stubbed (no Binder, ADR-3)");
  }

  // P2.4/V3 (render): load acl's own android.graphics natives (libacl_graphics_jni, built
  // by Soong against Android's host libskia) and register them. Unlike the host
  // libandroid_runtime graphics umbrella (whose layoutlib build compiles @CriticalNative
  // natives with the wrong (env,clazz) signature → ABI crash), acl's natives use the DEVICE
  // convention, matching the framework's @CriticalNative that host ART honors. So real
  // android.graphics.Bitmap/Canvas/Paint execute on our (Android's) Skia. Opt-in via
  // ACL_ART_GRAPHICS_LIB (path to libacl_graphics_jni.so); best-effort.
  void load_acl_graphics(JNIEnv* env) {
    const char* lib = std::getenv("ACL_ART_GRAPHICS_LIB");
    if (lib == nullptr || *lib == '\0') return;
    void* h = ::dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
      const char* e = ::dlerror();
      log::w(kTag, "acl graphics: dlopen({}) failed: {}", lib, e ? e : "?");
      return;
    }
    auto reg = reinterpret_cast<jint (*)(JNIEnv*)>(::dlsym(h, "register_acl_graphics"));
    if (reg == nullptr) {
      log::w(kTag, "acl graphics: register_acl_graphics not found in {}", lib);
      return;
    }
    const jint rc = reg(env);
    if (env->ExceptionCheck() || rc != JNI_OK) {
      env->ExceptionClear();
      log::w(kTag, "acl graphics: registration failed (rc={})", rc);
      return;
    }
    log::i(kTag,
           "acl graphics: android.graphics natives registered on Android host Skia "
           "(Bitmap/Canvas/Paint)");
    // AssetManager.openFd: the host/layoutlib libandroid_runtime stubs nativeOpenAssetFd/
    // nativeOpenNonAssetFd ("Implement me"). Override them with acl's real impl (same libandroidfw
    // this lib links) so openFd returns a real fd — libGDX Pixmap length()/AssetFileDescriptor,
    // SoundPool/MediaPlayer AssetFileDescriptor sources, etc.
    if (auto reg_fd = reinterpret_cast<jint (*)(JNIEnv*)>(::dlsym(h, "register_acl_asset_fd"))) {
      if (reg_fd(env) == JNI_OK && !env->ExceptionCheck())
        log::i(kTag, "acl asset: AssetManager.openFd natives registered (host layoutlib stub replaced)");
      else
        env->ExceptionClear();
    }
    // Font seam: bind org.acl.internal.HostBootstrap.nTypefaceFromPath to the graphics lib's
    // acl_typeface_from_path (SkTypeface::MakeFromFile), so seedTypefaces can load distinct
    // system families (sans/serif/monospace) from real font files without the framework's
    // unavailable createFromFile / @CriticalNative Font$Builder path. See docs/font-scope.md.
    g_acl_typeface_from_path =
        reinterpret_cast<jlong (*)(const char*)>(::dlsym(h, "acl_typeface_from_path"));
    if (g_acl_typeface_from_path != nullptr) {
      if (jclass boot = find_class_optional(env, "org/acl/internal/HostBootstrap"); boot != nullptr) {
        JNINativeMethod m[] = {
            {const_cast<char*>("nTypefaceFromPath"), const_cast<char*>("(Ljava/lang/String;)J"),
             reinterpret_cast<void*>(
                 +[](JNIEnv* e, jclass, jstring jp) -> jlong {
                   if (g_acl_typeface_from_path == nullptr || jp == nullptr) return 0;
                   const char* p = e->GetStringUTFChars(jp, nullptr);
                   const jlong hnd = p != nullptr ? g_acl_typeface_from_path(p) : 0;
                   if (p != nullptr) e->ReleaseStringUTFChars(jp, p);
                   return hnd;
                 })},
        };
        if (env->RegisterNatives(boot, m, 1) != JNI_OK) env->ExceptionClear();
        env->DeleteLocalRef(boot);
      } else {
        env->ExceptionClear();
      }
    }
    graphics_ops_check(env);
    graphics_present_demo(env);
    graphics_view_demo(env);
  }

  // GLSurfaceView Stage 2b: load acl's android.opengl.GLES20/GLES30 natives (libacl_gles_jni,
  // AOSP's glgen passthrough built by Soong) and register them onto the framework classes. The
  // gl* calls bind to the host Mesa libGLESv2 acl already loads (RTLD_GLOBAL via gfx/gl_egl) —
  // the same GL context the EGLImpl JNI makes current. Opt-in via ACL_ART_GLES_LIB (path to
  // libacl_gles_jni.so); best-effort. A GLSurfaceView app's GLThread issues these once it has
  // an EGL context+surface (com.google.android.gles_jni.EGLImpl, Stage 2a) current.
  void load_acl_gles(JNIEnv* env) {
    const char* lib = std::getenv("ACL_ART_GLES_LIB");
    if (lib == nullptr || *lib == '\0') return;
    // The vendor lib leaves its gl*/egl* calls undefined (the AOSP host sysroot has no Mesa),
    // so bring the system Mesa GLESv2/EGL into the process' GLOBAL scope FIRST — the vendor
    // lib's undefined gl* then bind to them, and they're the same GL that gfx/gl_egl (the
    // EGLImpl context) uses. Best-effort: if absent, the vendor dlopen below fails loudly.
    ::dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    ::dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    ::dlopen("libGLESv1_CM.so.1", RTLD_NOW | RTLD_GLOBAL);  // GLImpl's GLES1 (GL10) symbols
    // RTLD_LAZY (not NOW): GLImpl's glgen table references the full GLES1 OES surface
    // (glDrawTexfOES, glColorPointerBounds, …) that desktop Mesa doesn't export, but a GL20 app
    // never calls them — lazy binding leaves them unresolved harmlessly instead of failing load.
    void* h = ::dlopen(lib, RTLD_LAZY | RTLD_GLOBAL);
    if (h == nullptr) {
      const char* e = ::dlerror();
      log::w(kTag, "acl gles: dlopen({}) failed: {}", lib, e ? e : "?");
      return;
    }
    auto reg = reinterpret_cast<jint (*)(JNIEnv*)>(::dlsym(h, "register_acl_gles"));
    if (reg == nullptr) {
      log::w(kTag, "acl gles: register_acl_gles not found in {}", lib);
      return;
    }
    const jint rc = reg(env);
    if (env->ExceptionCheck() || rc != JNI_OK) {
      env->ExceptionClear();
      log::w(kTag, "acl gles: registration failed (rc={})", rc);
      return;
    }
    log::i(kTag, "acl gles: android.opengl.GLES20/GLES30 natives registered on host Mesa libGLESv2");
  }

  // GLSurfaceView Stage 2a: register com.google.android.gles_jni.EGLImpl on host Mesa libEGL,
  // over gfx::GlDevice. The EGL10 the app's GLThread drives (eglGetDisplay/Initialize/Choose
  // Config/CreateContext/MakeCurrent/SwapBuffers) now runs. Two seams via hooks: get_display
  // returns the device's EGLDisplay (lazily created); create_window_surface (Surface ->
  // wl_surface) is Stage 3, so it reports the gap for now. GL apps only — the device is created
  // lazily on the first eglGetDisplay, so non-GL apps pay nothing.
  void load_acl_egl10([[maybe_unused]] JNIEnv* env) {
#if ACL_HAVE_GL
    g_gl_main_thread = std::this_thread::get_id();  // boot thread = app main/UI thread
    env->GetJavaVM(&g_present_vm);                   // for the get_display in-place session creation
    g_present_app_id = config_.package_name.empty() ? "org.acl.app" : config_.package_name;
    gfx::AclEgl10Hooks hooks;
    hooks.get_display = [](void*) -> EGLDisplay {
      // The app's GLThread (NOT the main thread) blocks up to 10s for present_glsurfaceview to
      // publish the wayland device, so its context is created on the SAME device as its window
      // surface (else eglMakeCurrent EGL_BAD_MATCH). The main thread's checkGL20 probe never
      // blocks (present only runs after onCreate) — it takes the headless gbm fallback below.
      if (std::this_thread::get_id() != g_gl_main_thread) {
        std::unique_lock<std::mutex> lk(g_gl_session.mtx);
        g_gl_session.cv.wait_for(lk, std::chrono::seconds(10), [] { return g_gl_session.ready; });
        if (g_gl_session.ready && g_gl_session.device != nullptr)
          return static_cast<EGLDisplay>(g_gl_session.device->egl_display());
      } else if (g_gl_session.device != nullptr) {
        return static_cast<EGLDisplay>(g_gl_session.device->egl_display());
      }
      // Headless gbm fallback: the checkGL20 probe, or an app that uses EGL without a GLSurfaceView.
      static std::unique_ptr<gfx::GlDevice> gbm = [] {
        std::string err;
        auto d = gfx::GlDevice::CreateGbm(&err);
        if (!d) log::w(kTag, "egl10: fallback GL device unavailable: {}", err);
        return d;
      }();
      return gbm ? static_cast<EGLDisplay>(gbm->egl_display()) : EGL_NO_DISPLAY;
    };
    hooks.create_window_surface = [](JNIEnv*, jobject, EGLConfig config, const EGLint*, void*) -> EGLSurface {
      // Route (a): the app's GLThread asks for a window surface on its GLSurfaceView's Surface;
      // for the fullscreen first cut there is one GL surface = the toplevel wl_surface. Ignore
      // the native_window jobject identity and back it with the session's toplevel. Pass the
      // app's chosen `config` so the surface matches its context (eglMakeCurrent).
      if (g_gl_session.device == nullptr || g_gl_session.wl_surface == nullptr) {
        log::w(kTag, "egl10: eglCreateWindowSurface with no active GL present session");
        return EGL_NO_SURFACE;
      }
      // One wl_egl_window per toplevel wl_surface: drop any prior window before backing a new
      // surface, so a surface recreation never leaves two wl_egl_windows on the wl_surface (which
      // makes the new surface EGL_BAD_SURFACE at eglMakeCurrent). See the native-egl hook below.
      g_gl_session.window.reset();
      std::string err;
      auto win = g_gl_session.device->make_window(
          g_gl_session.w, g_gl_session.h, g_gl_session.wl_surface, {}, &err, config);
      if (!win) { log::w(kTag, "egl10: make_window failed: {}", err); return EGL_NO_SURFACE; }
      auto* egl = static_cast<EGLSurface>(win->egl_surface());
      g_gl_session.window = std::move(win);  // keep the wl_egl_window alive
      log::i(kTag, "egl10: eglCreateWindowSurface -> GL window on acl's toplevel wl_surface");
      return egl;
    };
    if (gfx::register_egl10(env, hooks) == JNI_OK)
      log::i(kTag, "egl10: EGLImpl registered (GLThread context + window-surface path live)");

    // Phase 3: back the app's NATIVE EGL (Flutter/Impeller, via the bionic libEGL forwarders) with
    // the SAME GL session as egl10 — acl's wl_display device + the app's wl_surface. Mirrors the two
    // hooks above; runs under the GL donor %fs, so the callbacks stay lock-light and TLS-free.
    acl::ndk::NativeGlHooks nhooks;
    nhooks.get_display = [](void*) -> void* {
      // Fast path: the present session is already published (nGlOpen ran, or a prior get_display
      // created it) — return its display.
      {
        std::unique_lock<std::mutex> lk(g_gl_session.mtx);
        if (g_gl_session.ready && g_gl_session.device != nullptr)
          return static_cast<void*>(g_gl_session.device->egl_display());
      }
      // The newer Flutter engine builds its GL environment AND rendering surface SYNCHRONOUSLY on
      // its raster thread DURING onCreate, while the app main thread is blocked in the engine ctor —
      // before acl's post-onCreate nGlOpen would publish the session. Blocking here then deadlocks
      // (the main thread can't reach nGlOpen). Since get_display is only ever called by a native-EGL
      // (Flutter) app, create the full wayland session in place now; nGlOpen adopts it once the decor
      // exists. The wl_display's main event queue is dispatched by one thread at a time (this
      // roundtrip completes before runGl's Looper starts), and Mesa drives its own queue from the
      // raster thread's eglSwapBuffers — the same split as the working post-onCreate path, so the
      // creating thread does not matter. A gbm fallback would not do — the engine's offscreen surface
      // needs a config from the real session device.
      if (g_present_vm != nullptr) {
        std::lock_guard<std::mutex> plk(g_pending_gl_mtx);
        if (g_pending_gl_session == nullptr) {
          const int w = svc::window().display_width(), h = svc::window().display_height();
          log::i(kTag, "native egl: get_display creating GL session in place ({}x{}) to break the "
                       "engine onCreate<->nGlOpen deadlock", w, h);
          g_pending_gl_session = create_gl_session_core(g_present_vm, w, h, g_present_app_id, g_present_app_id);
        }
        std::unique_lock<std::mutex> lk(g_gl_session.mtx);
        if (g_gl_session.ready && g_gl_session.device != nullptr)
          return static_cast<void*>(g_gl_session.device->egl_display());
        log::w(kTag, "native egl: get_display in-place session creation FAILED -> engine BAD_DISPLAY");
        return nullptr;
      }
      log::w(kTag, "native egl: get_display has no app JavaVM -> engine BAD_DISPLAY");
      return nullptr;
    };
    nhooks.create_window_surface = [](void*, void* cfg) -> void* {
      std::lock_guard<std::mutex> lk(g_gl_session.mtx);
      if (g_gl_session.device == nullptr || g_gl_session.wl_surface == nullptr) {
        log::w(kTag, "native egl: eglCreateWindowSurface with no active GL session");
        return nullptr;
      }
      // Impeller recreates its onscreen surface on each SurfaceHolder callback (surfaceCreated then
      // surfaceChanged), destroying the prior EGLSurface (eglDestroySurface -> Mesa) in between. acl
      // has a SINGLE toplevel wl_surface, and a wl_surface can back only one wl_egl_window at a time:
      // creating the new window while the old one still wraps the toplevel yields a surface Mesa then
      // rejects (eglMakeCurrent -> EGL_BAD_SURFACE). Tear the old window down FIRST so the new
      // wl_egl_window is the sole occupant of the wl_surface.
      g_gl_session.window.reset();
      std::string err;
      auto win = g_gl_session.device->make_window(g_gl_session.w, g_gl_session.h,
                                                  g_gl_session.wl_surface, {}, &err,
                                                  static_cast<EGLConfig>(cfg));
      if (!win) { log::w(kTag, "native egl: make_window failed: {}", err); return nullptr; }
      void* egl = static_cast<void*>(win->egl_surface());
      g_gl_session.window = std::move(win);  // keep the wl_egl_window alive
      log::i(kTag, "native egl: eglCreateWindowSurface -> GL window on acl's toplevel wl_surface");
      return egl;
    };
    nhooks.window_size = [](void*, int* w, int* h) {  // captureless + lock-free: safe under bionic %fs
      if (w) *w = g_gl_session.w;
      if (h) *h = g_gl_session.h;
    };
    // Flutter external textures: the engine imports an app/producer AHardwareBuffer as an EGLImage
    // via eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ahb), which host Mesa can't consume. Point the
    // forwarder's translation hook at gfx::egl_image_from_ahb (dma-buf import). The typed signature is
    // ABI-compatible with the void*-typed hook slot (all params are pointers).
    nhooks.egl_image_from_ahb = reinterpret_cast<void* (*)(void*, const void*, const int*, void*)>(
        &acl::gfx::egl_image_from_ahb);
    acl::ndk::set_native_gl_hooks(nhooks);
    // Back libandroid's AHardwareBuffer_{describe,acquire,release} with gfx::ahb_*. The typed
    // signatures are ABI-compatible with the void*-typed op slots (AHardwareBuffer*/Desc* are pointers).
    acl::ndk::set_ahb_ops(
        reinterpret_cast<void (*)(const void*, void*)>(&acl::gfx::ahb_describe),
        reinterpret_cast<void (*)(void*)>(&acl::gfx::ahb_acquire),
        reinterpret_cast<void (*)(void*)>(&acl::gfx::ahb_release));
#endif
  }

  // App path (GPU): host a REAL android.view.View and present it GPU-native. The app
  // provides ONLY a View subclass (ACL_ART_VIEW_HOST) — like a real app's custom View. acl's
  // runtime bootstrap (org.acl.internal.HostBootstrap) supplies the framework environment
  // (main Looper, no-Binder ServiceManager, minimal Context); acl constructs the View,
  // measures/lays it out, and drives it through the Ganesh Vulkan present loop (begin_frame →
  // View.draw on the GPU Canvas → end_frame → tiled dma-buf → Wayland). No software path.
  void graphics_view_demo([[maybe_unused]] JNIEnv* env) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    const char* host = std::getenv("ACL_ART_VIEW_HOST");
    if (host == nullptr || *host == '\0') return;
    constexpr int W = 480, H = 320;

    // Runtime bootstrap: main Looper + no-Binder ServiceManager + a Context factory.
    jclass boot = env->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) {
      env->ExceptionClear();
      log::w(kTag, "view host: HostBootstrap not on classpath (ACL_ART_HOST_BOOTSTRAP?)");
      return;
    }
    env->CallStaticVoidMethod(boot, env->GetStaticMethodID(boot, "init", "()V"));
    jobject ctx = env->CallStaticObjectMethod(
        boot, env->GetStaticMethodID(boot, "newContext", "()Landroid/content/Context;"));
    if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }

    jclass vcls = env->FindClass(to_jni_name(host).c_str());
    if (vcls == nullptr) { env->ExceptionClear(); log::w(kTag, "view host: {} not found", host); env->DeleteLocalRef(boot); return; }

    // Activity probe: real apps enter via an Activity (onCreate → setContentView), not a
    // directly-constructible View. Map the first wall of the Activity/Window frontier.
    if (jclass actC = env->FindClass("android/app/Activity")) {
      const bool is_activity = env->IsAssignableFrom(vcls, actC) == JNI_TRUE;
      env->DeleteLocalRef(actC);
      if (is_activity) {
        log::i(kTag, "view host: {} is an Activity — attach + onCreate", host);
        jobject act = env->NewObject(vcls, env->GetMethodID(vcls, "<init>", "()V"));
        // Lightweight attach (HostBootstrap: mActivityInfo + PhoneWindow), then onCreate.
        env->CallStaticVoidMethod(
            boot,
            env->GetStaticMethodID(boot, "attachActivity",
                                   "(Landroid/app/Activity;Landroid/content/Context;)V"),
            act, ctx);
        if (act != nullptr && env->ExceptionCheck() != JNI_TRUE) {
          env->CallVoidMethod(act, env->GetMethodID(vcls, "onCreate", "(Landroid/os/Bundle;)V"),
                              static_cast<jobject>(nullptr));
        }
        if (env->ExceptionCheck() == JNI_TRUE) {
          env->ExceptionDescribe();
          env->ExceptionClear();
          log::w(kTag, "view host: Activity attach/onCreate hit a wall (Window/attach frontier; "
                       "docs/app-path-scope.md)");
          if (act != nullptr) env->DeleteLocalRef(act);
          env->DeleteLocalRef(vcls);
          env->DeleteLocalRef(boot);
          return;
        }
        // Content view = the Window's decor view (root of the installed hierarchy).
        jclass actClass = env->GetObjectClass(act);
        jobject window = env->CallObjectMethod(
            act, env->GetMethodID(actClass, "getWindow", "()Landroid/view/Window;"));
        jobject decor = window != nullptr
            ? env->CallObjectMethod(window, env->GetMethodID(env->GetObjectClass(window),
                                    "getDecorView", "()Landroid/view/View;"))
            : nullptr;
        if (decor == nullptr || env->ExceptionCheck() == JNI_TRUE) {
          if (env->ExceptionCheck()) env->ExceptionDescribe();
          env->ExceptionClear();
          log::w(kTag, "view host: Activity has no decor view to present");
          env->DeleteLocalRef(act);
          env->DeleteLocalRef(vcls);
          env->DeleteLocalRef(boot);
          return;
        }
        jclass dcls = env->GetObjectClass(decor);
        constexpr jint kExactlyA = 1 << 30;
        env->CallVoidMethod(decor, env->GetMethodID(dcls, "measure", "(II)V"),
                            static_cast<jint>((W & 0x3FFFFFFF) | kExactlyA),
                            static_cast<jint>((H & 0x3FFFFFFF) | kExactlyA));
        env->CallVoidMethod(decor, env->GetMethodID(dcls, "layout", "(IIII)V"), 0, 0, W, H);
        if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }
        log::i(kTag, "view host: Activity {} content view ready; presenting GPU-native", host);
        present_content_loop_gpu(decor, W, H);
        env->DeleteLocalRef(act);
        env->DeleteLocalRef(vcls);
        env->DeleteLocalRef(boot);
        return;
      }
    } else {
      env->ExceptionClear();
    }

    jmethodID vctor = vcls ? env->GetMethodID(vcls, "<init>", "(Landroid/content/Context;)V")
                           : nullptr;
    jobject view = vctor ? env->NewObject(vcls, vctor, ctx) : nullptr;
    if (view == nullptr || env->ExceptionCheck() == JNI_TRUE) {
      if (env->ExceptionCheck()) env->ExceptionDescribe();
      env->ExceptionClear();
      log::w(kTag, "view host: could not construct View {}", host);
      env->DeleteLocalRef(boot);
      return;
    }
    // measure(EXACTLY W, EXACTLY H) + layout(0,0,W,H) — MeasureSpec.EXACTLY == 1<<30.
    constexpr jint kExactly = 1 << 30;
    jmethodID measure = env->GetMethodID(vcls, "measure", "(II)V");
    jmethodID layout = env->GetMethodID(vcls, "layout", "(IIII)V");
    env->CallVoidMethod(view, measure, static_cast<jint>((W & 0x3FFFFFFF) | kExactly),
                        static_cast<jint>((H & 0x3FFFFFFF) | kExactly));
    env->CallVoidMethod(view, layout, 0, 0, W, H);
    if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }

    log::i(kTag, "view host: {} constructed + laid out; presenting GPU-native", host);
    present_content_loop_gpu(view, W, H);
    env->DeleteLocalRef(view);
    env->DeleteLocalRef(vcls);
    env->DeleteLocalRef(boot);
#endif
  }

  // Exercise the Canvas transform/clip + geometry natives on a REAL android.graphics.Canvas
  // (Android host Skia) and verify with pixel readback. Gated by ACL_ART_GRAPHICS_OPS.
  // Each new native is called (so a missing registration surfaces as UnsatisfiedLinkError)
  // and a few pixels are asserted (fill circle, stroke, translate+clip).
  void graphics_ops_check([[maybe_unused]] JNIEnv* env) {
    const char* on = std::getenv("ACL_ART_GRAPHICS_OPS");
    if (on == nullptr || *on == '\0') return;
    constexpr int W = 64, H = 64;
    const auto jfail = [&](const char* what) {
      if (env->ExceptionCheck()) env->ExceptionDescribe();
      env->ExceptionClear();
      log::w(kTag, "graphics ops: {} failed", what);
    };
    jclass bitmapC = env->FindClass("android/graphics/Bitmap");
    jclass configC = env->FindClass("android/graphics/Bitmap$Config");
    jclass canvasC = env->FindClass("android/graphics/Canvas");
    jclass paintC = env->FindClass("android/graphics/Paint");
    if (bitmapC == nullptr || configC == nullptr || canvasC == nullptr || paintC == nullptr)
      return jfail("FindClass");
    jobject argb = env->GetStaticObjectField(
        configC, env->GetStaticFieldID(configC, "ARGB_8888", "Landroid/graphics/Bitmap$Config;"));
    jobject bmp = env->CallStaticObjectMethod(
        bitmapC,
        env->GetStaticMethodID(bitmapC, "createBitmap",
                               "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"),
        W, H, argb);
    if (bmp == nullptr) return jfail("createBitmap");
    jobject canvas = env->NewObject(
        canvasC, env->GetMethodID(canvasC, "<init>", "(Landroid/graphics/Bitmap;)V"), bmp);
    if (canvas == nullptr) return jfail("new Canvas");

    const jmethodID drawColor = env->GetMethodID(canvasC, "drawColor", "(I)V");
    const jmethodID drawCircle = env->GetMethodID(canvasC, "drawCircle", "(FFFLandroid/graphics/Paint;)V");
    const jmethodID drawRect = env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V");
    const jmethodID save = env->GetMethodID(canvasC, "save", "()I");
    const jmethodID restore = env->GetMethodID(canvasC, "restore", "()V");
    const jmethodID translate = env->GetMethodID(canvasC, "translate", "(FF)V");
    const jmethodID clipRect = env->GetMethodID(canvasC, "clipRect", "(FFFF)Z");
    const jmethodID setColor = env->GetMethodID(paintC, "setColor", "(I)V");
    const jmethodID setStyle = env->GetMethodID(paintC, "setStyle", "(Landroid/graphics/Paint$Style;)V");
    if (drawCircle == nullptr || save == nullptr || translate == nullptr || clipRect == nullptr ||
        setStyle == nullptr)
      return jfail("GetMethodID (ops)");

    // Black background; a red FILL circle at the center; then translate+clip a green rect.
    env->CallVoidMethod(canvas, drawColor, static_cast<jint>(0xFF000000));
    jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
    jobject fillStyle = env->GetStaticObjectField(
        env->FindClass("android/graphics/Paint$Style"),
        env->GetStaticFieldID(env->FindClass("android/graphics/Paint$Style"), "FILL",
                              "Landroid/graphics/Paint$Style;"));
    env->CallVoidMethod(paint, setStyle, fillStyle);
    env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFFFF0000));  // red
    env->CallVoidMethod(canvas, drawCircle, 32.0f, 32.0f, 16.0f, paint);

    env->CallIntMethod(canvas, save);
    env->CallVoidMethod(canvas, translate, 4.0f, 4.0f);
    env->CallBooleanMethod(canvas, clipRect, 0.0f, 0.0f, 8.0f, 8.0f);  // device (4,4)-(12,12)
    env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFF00FF00));  // green
    env->CallVoidMethod(canvas, drawRect, -100.0f, -100.0f, 100.0f, 100.0f, paint);  // clipped
    env->CallVoidMethod(canvas, restore);
    if (env->ExceptionCheck()) return jfail("draw ops");

    // Text: white "A" in the clear top-right region (y<16 is above the circle) — verifies
    // the SkFont/drawText path (nSetTextSize + nDrawText).
    const jmethodID setTextSize = env->GetMethodID(paintC, "setTextSize", "(F)V");
    const jmethodID drawText = env->GetMethodID(
        canvasC, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    bool text_ok_call = false;
    if (setTextSize != nullptr && drawText != nullptr) {
      env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFFFFFFFF));  // white
      env->CallVoidMethod(paint, setTextSize, 18.0f);
      jstring s = env->NewStringUTF("A");
      env->CallVoidMethod(canvas, drawText, s, 44.0f, 14.0f, paint);  // baseline (44,14)
      env->DeleteLocalRef(s);
      text_ok_call = env->ExceptionCheck() == JNI_FALSE;
    }
    env->ExceptionClear();

    // drawBitmap: an 8×8 magenta bitmap at (2,40) — clear region left of the circle.
    bool bmp_ok_call = false;
    jobject src = env->CallStaticObjectMethod(
        bitmapC,
        env->GetStaticMethodID(bitmapC, "createBitmap",
                               "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"),
        8, 8, argb);
    if (src != nullptr) {
      env->CallVoidMethod(src, env->GetMethodID(bitmapC, "eraseColor", "(I)V"),
                          static_cast<jint>(0xFFFF00FF));  // magenta
      env->CallVoidMethod(
          canvas,
          env->GetMethodID(canvasC, "drawBitmap",
                           "(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V"),
          src, 2.0f, 40.0f, static_cast<jobject>(nullptr));
      bmp_ok_call = env->ExceptionCheck() == JNI_FALSE;
      env->DeleteLocalRef(src);
    }
    env->ExceptionClear();

    // measureText / getTextBounds on "Ag".
    float advance = 0.0f;
    int bounds_w = 0;
    if (jmethodID measure = env->GetMethodID(paintC, "measureText", "(Ljava/lang/String;)F")) {
      jstring t = env->NewStringUTF("Ag");
      advance = env->CallFloatMethod(paint, measure, t);
      jclass rectC = env->FindClass("android/graphics/Rect");
      jobject rb = env->NewObject(rectC, env->GetMethodID(rectC, "<init>", "()V"));
      if (jmethodID gtb = env->GetMethodID(
              paintC, "getTextBounds", "(Ljava/lang/String;IILandroid/graphics/Rect;)V")) {
        env->CallVoidMethod(paint, gtb, t, 0, 2, rb);
        bounds_w = env->CallIntMethod(rb, env->GetMethodID(rectC, "width", "()I"));
      }
      env->DeleteLocalRef(t);
      env->DeleteLocalRef(rb);
      env->DeleteLocalRef(rectC);
    }
    env->ExceptionClear();

    // Path: a filled cyan triangle in the bottom-right (clear of the circle at x<48).
    bool path_ok_call = false;
    if (jclass pathC = env->FindClass("android/graphics/Path")) {
      jobject path = env->NewObject(pathC, env->GetMethodID(pathC, "<init>", "()V"));
      const jmethodID moveTo = env->GetMethodID(pathC, "moveTo", "(FF)V");
      const jmethodID lineTo = env->GetMethodID(pathC, "lineTo", "(FF)V");
      const jmethodID close = env->GetMethodID(pathC, "close", "()V");
      const jmethodID drawPath = env->GetMethodID(
          canvasC, "drawPath", "(Landroid/graphics/Path;Landroid/graphics/Paint;)V");
      if (path != nullptr && moveTo != nullptr && drawPath != nullptr) {
        env->CallVoidMethod(path, moveTo, 50.0f, 50.0f);
        env->CallVoidMethod(path, lineTo, 62.0f, 50.0f);
        env->CallVoidMethod(path, lineTo, 56.0f, 62.0f);
        env->CallVoidMethod(path, close);
        env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFF00FFFF));  // cyan
        env->CallVoidMethod(canvas, drawPath, path, paint);
        path_ok_call = env->ExceptionCheck() == JNI_FALSE;
      }
      if (path != nullptr) env->DeleteLocalRef(path);
      env->DeleteLocalRef(pathC);
    }
    env->ExceptionClear();

    // Matrix: concat a 2×-scale + translate (via setValues) and draw a rect through it —
    // rect(0,0,3,3) → device (2,52)-(8,58), a clear region below the bitmap and circle.
    bool matrix_ok_call = false;
    if (jclass matrixC = env->FindClass("android/graphics/Matrix")) {
      jobject m = env->NewObject(matrixC, env->GetMethodID(matrixC, "<init>", "()V"));
      const jmethodID setValues = env->GetMethodID(matrixC, "setValues", "([F)V");
      const jmethodID concat =
          env->GetMethodID(canvasC, "concat", "(Landroid/graphics/Matrix;)V");
      if (m != nullptr && setValues != nullptr && concat != nullptr) {
        const jfloat vals[9] = {2, 0, 2, 0, 2, 52, 0, 0, 1};  // scale 2, translate (2,52)
        jfloatArray arr = env->NewFloatArray(9);
        env->SetFloatArrayRegion(arr, 0, 9, vals);
        env->CallVoidMethod(m, setValues, arr);
        env->CallIntMethod(canvas, save);
        env->CallVoidMethod(canvas, concat, m);
        env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFFFFFF00));  // yellow
        env->CallVoidMethod(canvas, drawRect, 0.0f, 0.0f, 3.0f, 3.0f, paint);
        env->CallVoidMethod(canvas, restore);
        matrix_ok_call = env->ExceptionCheck() == JNI_FALSE;
        env->DeleteLocalRef(arr);
      }
      if (m != nullptr) env->DeleteLocalRef(m);
      env->DeleteLocalRef(matrixC);
    }
    env->ExceptionClear();

    // Gradient: a horizontal red→blue LinearGradient filling a rect in the left strip
    // (x0..14, y16..40) — clear of the circle (x≥16) and bitmap (y≥40).
    bool grad_ok_call = false;
    if (jclass lgC = env->FindClass("android/graphics/LinearGradient")) {
      jclass tmC = env->FindClass("android/graphics/Shader$TileMode");
      jobject clamp = env->GetStaticObjectField(
          tmC, env->GetStaticFieldID(tmC, "CLAMP", "Landroid/graphics/Shader$TileMode;"));
      jmethodID lgCtor =
          env->GetMethodID(lgC, "<init>", "(FFFFIILandroid/graphics/Shader$TileMode;)V");
      jmethodID setShader = env->GetMethodID(
          paintC, "setShader", "(Landroid/graphics/Shader;)Landroid/graphics/Shader;");
      if (lgCtor != nullptr && setShader != nullptr) {
        jobject grad = env->NewObject(lgC, lgCtor, 0.0f, 28.0f, 14.0f, 28.0f,
                                      static_cast<jint>(0xFFFF0000), static_cast<jint>(0xFF0000FF),
                                      clamp);
        if (grad != nullptr) {
          env->CallObjectMethod(paint, setShader, grad);
          env->CallVoidMethod(canvas, drawRect, 0.0f, 16.0f, 14.0f, 40.0f, paint);
          env->CallObjectMethod(paint, setShader, static_cast<jobject>(nullptr));  // clear
          grad_ok_call = env->ExceptionCheck() == JNI_FALSE;
          env->DeleteLocalRef(grad);
        }
      }
      env->DeleteLocalRef(tmC);
      env->DeleteLocalRef(lgC);
    }
    env->ExceptionClear();

    auto read = reinterpret_cast<bool (*)(long, std::uint32_t*, int, int)>(
        ::dlsym(RTLD_DEFAULT, "acl_read_bitmap"));
    if (read == nullptr) { log::w(kTag, "graphics ops: acl_read_bitmap missing"); return; }
    std::vector<std::uint32_t> px(static_cast<std::size_t>(W) * H);
    const jlong handle = env->CallLongMethod(
        bmp, env->GetMethodID(bitmapC, "getNativeInstance", "()J"));
    if (!read(handle, px.data(), W, H)) { log::w(kTag, "graphics ops: readback failed"); return; }
    const auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * W + x]; };
    const bool center_red = at(32, 32) == 0xFFFF0000u;
    const bool bg_black = at(2, 2) == 0xFF000000u;         // outside circle + clip
    const bool clip_green = at(6, 6) == 0xFF00FF00u;       // inside translated clip (4,4)-(12,12)
    const bool clip_edge = at(2, 2) != 0xFF00FF00u;        // outside the clip stayed black
    // Count non-black pixels in the glyph region (x 40..64, y 0..14) — above the circle
    // (top y=16) and right of the clip (x<12), so any non-black = a rasterized glyph.
    int text_px = 0;
    for (int y = 0; y < 15; ++y)
      for (int x = 40; x < W; ++x)
        if (at(x, y) != 0xFF000000u) ++text_px;
    const bool text_ok = text_ok_call && text_px > 0;
    const bool bmp_ok = bmp_ok_call && at(4, 42) == 0xFFFF00FFu;  // magenta bitmap at (2,40)
    const bool measure_ok = advance > 0.0f && bounds_w > 0;
    const bool path_ok = path_ok_call && at(55, 53) == 0xFF00FFFFu;  // cyan inside the triangle
    const bool matrix_ok = matrix_ok_call && at(5, 55) == 0xFFFFFF00u;  // yellow in scaled rect
    // Gradient: left end reddish (R>B), right end bluish (B>R).
    const auto R = [](std::uint32_t p) { return (p >> 16) & 0xffu; };
    const auto B = [](std::uint32_t p) { return p & 0xffu; };
    const bool grad_ok = grad_ok_call && R(at(1, 28)) > B(at(1, 28)) && B(at(12, 28)) > R(at(12, 28));
    log::i(kTag,
           "graphics ops: circle={} clip={} text={} ({}px) bitmap={} measure={} path={} "
           "matrix={} gradient={} (l=0x{:08x} r=0x{:08x})",
           center_red, clip_green, text_ok, text_px, bmp_ok, measure_ok, path_ok, matrix_ok,
           grad_ok, at(1, 28), at(12, 28));
    log::i(kTag,
           "graphics ops: geometry + text + bitmap + measure + path + matrix + gradient {} "
           "(real Canvas, Android Skia)",
           (center_red && bg_black && clip_green && clip_edge && text_ok && bmp_ok && measure_ok &&
            path_ok && matrix_ok && grad_ok)
               ? "PASS"
               : "MISMATCH");
    env->DeleteLocalRef(canvas);
    env->DeleteLocalRef(bmp);
  }

  // S3b (P2.4): the full loop, as a live demo. Gated by ACL_ART_GRAPHICS_DEMO. Drives a
  // real android.graphics render via JNI (Bitmap/Canvas/Paint on Android's Skia), reads
  // the Bitmap's pixels (libacl_graphics_jni acl_read_bitmap) into a gfx::Surface, imports
  // them into a dma-buf (gfx VkRenderTarget), and presents to the Wayland compositor
  // (zwp_linux_dmabuf_v1). Closes app→android.graphics→Android Skia→dma-buf→Wayland.
  void graphics_present_demo([[maybe_unused]] JNIEnv* env) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    const char* on = std::getenv("ACL_ART_GRAPHICS_DEMO");
    if (on == nullptr || *on == '\0') return;
    constexpr int W = 480, H = 320;
    constexpr jint kBg = 0xFF204060, kFg = 0xFFCC8844;

    struct AclDmabuf {
      int fd;
      std::uint32_t width, height, plane_count;
      std::uint32_t offsets[4], strides[4];
      std::uint64_t modifier;
      std::uint32_t drm_format;
    };
    // Present a once-rendered dma-buf frame N times (static demo scene).
    const auto present_frame = [&](const AclDmabuf& f, const char* title) -> bool {
      wm::DmabufRenderFn present = [&](const std::vector<std::uint64_t>&,
                                       gfx::DmabufFrame* out) -> bool {
        gfx::DmabufFrame fr{};
        fr.fd = f.fd;
        fr.width = f.width;
        fr.height = f.height;
        fr.plane_count = f.plane_count;
        for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
          fr.planes[i].offset = f.offsets[i];
          fr.planes[i].stride = f.strides[i];
        }
        fr.modifier = f.modifier;
        fr.drm_format = f.drm_format;
        fr.modifier_tiled = f.modifier != 0;
        *out = fr;
        return true;
      };
      return wm::present_dmabuf(present, title, "com.acl.graphics", 3);
    };

    // S4b step 2: ANIMATED GPU loop — a real android.graphics.Canvas re-draws a moving
    // rect each vsync tick onto the Ganesh Vulkan surface, presented via the per-frame
    // dma-buf loop (begin_frame → drawColor/drawRect → end_frame → present). Proves the
    // continuous GPU present path (present_dmabuf_loop) end-to-end. Gated by
    // ACL_ART_GRAPHICS_GPU_LOOP; frame count from the value (default 60).
    if (const char* gl = std::getenv("ACL_ART_GRAPHICS_GPU_LOOP");
        gl != nullptr && *gl != '\0') {
      const std::uint64_t frames = std::strtoull(gl, nullptr, 10) > 1
                                       ? std::strtoull(gl, nullptr, 10) : 60;
      auto begin = reinterpret_cast<long (*)(int, int)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
      auto finish = reinterpret_cast<bool (*)(int, AclDmabuf*)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
      if (begin == nullptr || finish == nullptr) {
        log::w(kTag, "graphics demo(gpu-loop): begin/end frame missing");
        return;
      }
      const jlong ch = begin(W, H);
      if (ch == 0) { log::w(kTag, "graphics demo(gpu-loop): begin_frame failed"); return; }
      jclass canvasC = env->FindClass("android/graphics/Canvas");
      jclass paintC = env->FindClass("android/graphics/Paint");
      if (canvasC == nullptr || paintC == nullptr) { env->ExceptionClear(); return; }
      jobject canvas = env->NewObject(
          canvasC, env->GetMethodID(canvasC, "<init>", "(J)V"), ch);
      jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
      if (canvas == nullptr || paint == nullptr) { env->ExceptionClear(); return; }
      const jmethodID drawColor = env->GetMethodID(canvasC, "drawColor", "(I)V");
      const jmethodID setColor = env->GetMethodID(paintC, "setColor", "(I)V");
      const jmethodID drawRect =
          env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V");
      env->CallVoidMethod(paint, setColor, kFg);
      const wm::DmabufFrameFn render = [&](int bufidx, const std::vector<std::uint64_t>&,
                                           std::uint64_t, std::uint64_t idx,
                                           gfx::DmabufFrame* out) -> bool {
        if (idx > 0 && begin(W, H) == 0) return false;  // re-arm + clear
        // A rect sweeping horizontally so successive frames visibly differ.
        const float x = 20.0f + static_cast<float>((idx * 8) % 360);
        env->CallVoidMethod(canvas, drawColor, kBg);
        env->CallVoidMethod(canvas, drawRect, x, 60.0f, x + 100.0f, 260.0f, paint);
        if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionClear(); return false; }
        AclDmabuf f{};
        if (!finish(bufidx, &f) || f.fd < 0) return false;
        gfx::DmabufFrame fr{};
        fr.fd = f.fd;
        fr.width = f.width;
        fr.height = f.height;
        fr.plane_count = f.plane_count;
        for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
          fr.planes[i].offset = f.offsets[i];
          fr.planes[i].stride = f.strides[i];
        }
        fr.modifier = f.modifier;
        fr.drm_format = f.drm_format;
        fr.modifier_tiled = f.modifier != 0;
        *out = fr;
        return true;
      };
      const wm::InputCallback no_input;
      const bool ok = wm::present_dmabuf_loop(W, H, "acl graphics (GPU loop)",
                                              "com.acl.graphics", render, no_input, false,
                                              frames);
      log::i(kTag, "graphics demo(gpu-loop): {} animated frame(s), real Canvas → Ganesh "
                   "Vulkan → dma-buf → Wayland {} (zero-copy)", frames,
             ok ? "presented" : "not presented");
      return;
    }

    // S4b: drive the app's REAL android.graphics.Canvas onto the GPU surface (not a
    // hardcoded C++ scene). Real Canvas ops → Android Skia Ganesh Vulkan → tiled dma-buf →
    // Wayland, zero-copy. Gated by ACL_ART_GRAPHICS_GPU_CANVAS.
    if (const char* gc = std::getenv("ACL_ART_GRAPHICS_GPU_CANVAS");
        gc != nullptr && *gc != '\0') {
      auto begin = reinterpret_cast<long (*)(int, int)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
      auto finish = reinterpret_cast<bool (*)(int, AclDmabuf*)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
      if (begin == nullptr || finish == nullptr) {
        log::w(kTag, "graphics demo(gpu-canvas): begin/end frame missing");
        return;
      }
      const auto jfail2 = [&](const char* what) {
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        env->ExceptionClear();
        log::w(kTag, "graphics demo(gpu-canvas): {} failed", what);
      };
      const jlong ch = begin(W, H);
      if (ch == 0) {
        log::w(kTag, "graphics demo(gpu-canvas): begin_frame failed");
        return;
      }
      jclass canvasC = env->FindClass("android/graphics/Canvas");
      jclass paintC = env->FindClass("android/graphics/Paint");
      if (canvasC == nullptr || paintC == nullptr) return jfail2("FindClass");
      jobject canvas = env->NewObject(
          canvasC, env->GetMethodID(canvasC, "<init>", "(J)V"), ch);
      if (canvas == nullptr) return jfail2("new Canvas(long)");
      env->CallVoidMethod(canvas, env->GetMethodID(canvasC, "drawColor", "(I)V"), kBg);
      jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
      env->CallVoidMethod(paint, env->GetMethodID(paintC, "setColor", "(I)V"), kFg);
      env->CallVoidMethod(
          canvas, env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V"),
          80.0f, 60.0f, 400.0f, 260.0f, paint);
      if (env->ExceptionCheck()) return jfail2("draw");
      AclDmabuf f{};
      if (!finish(0, &f) || f.fd < 0) {
        log::w(kTag, "graphics demo(gpu-canvas): end_frame failed");
        return;
      }
      log::i(kTag, "graphics demo(gpu-canvas): real android.graphics.Canvas → Ganesh Vulkan "
                   "→ dma-buf (modifier=0x{:x}, {} plane(s))", f.modifier, f.plane_count);
      const bool ok = present_frame(f, "acl graphics (GPU Canvas)");
      log::i(kTag, "graphics demo(gpu-canvas): real Canvas → Ganesh Vulkan → dma-buf → "
                   "Wayland {} (zero-copy)", ok ? "presented" : "not presented");
      return;
    }

    // S4a: GPU-native zero-copy path. libacl_graphics_jni renders the scene with Android's
    // Skia Ganesh Vulkan STRAIGHT into a modifier-tiled dma-buf VkImage (no CPU upload);
    // acl presents the exported fd. Gated by ACL_ART_GRAPHICS_GPU.
    if (const char* gpu = std::getenv("ACL_ART_GRAPHICS_GPU"); gpu != nullptr && *gpu != '\0') {
      auto render = reinterpret_cast<bool (*)(int, int, std::uint32_t, int, int, int, int,
                                              std::uint32_t, AclDmabuf*)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_render"));
      if (render == nullptr) {
        log::w(kTag, "graphics demo(gpu): acl_graphics_gpu_render missing");
        return;
      }
      AclDmabuf f{};
      if (!render(W, H, static_cast<std::uint32_t>(kBg), 80, 60, 400, 260,
                  static_cast<std::uint32_t>(kFg), &f) || f.fd < 0) {
        log::w(kTag, "graphics demo(gpu): GPU render failed");
        return;
      }
      log::i(kTag, "graphics demo(gpu): Android Skia Ganesh Vulkan rendered into dma-buf "
                   "(modifier=0x{:x}, {} plane(s))", f.modifier, f.plane_count);
      const bool ok = present_frame(f, "acl graphics (GPU)");
      log::i(kTag, "graphics demo(gpu): android.graphics Skia → Ganesh Vulkan → dma-buf → "
                   "Wayland {} (zero-copy)", ok ? "presented" : "not presented");
      return;
    }

    const auto jfail = [&](const char* what) {
      if (env->ExceptionCheck()) env->ExceptionDescribe();
      env->ExceptionClear();
      log::w(kTag, "graphics demo: {} failed", what);
    };

    // ── Drive a real android.graphics render ────────────────────────────────
    jclass bitmapC = env->FindClass("android/graphics/Bitmap");
    jclass configC = env->FindClass("android/graphics/Bitmap$Config");
    jclass canvasC = env->FindClass("android/graphics/Canvas");
    jclass paintC = env->FindClass("android/graphics/Paint");
    if (bitmapC == nullptr || configC == nullptr || canvasC == nullptr || paintC == nullptr)
      return jfail("FindClass");
    jfieldID argbF = env->GetStaticFieldID(configC, "ARGB_8888",
                                           "Landroid/graphics/Bitmap$Config;");
    jobject argb = env->GetStaticObjectField(configC, argbF);
    jmethodID createBitmap = env->GetStaticMethodID(
        bitmapC, "createBitmap",
        "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jobject bitmap = env->CallStaticObjectMethod(bitmapC, createBitmap, W, H, argb);
    if (bitmap == nullptr) return jfail("createBitmap");
    jobject canvas = env->NewObject(canvasC, env->GetMethodID(canvasC, "<init>",
                                    "(Landroid/graphics/Bitmap;)V"), bitmap);
    if (canvas == nullptr) return jfail("new Canvas");
    env->CallVoidMethod(canvas, env->GetMethodID(canvasC, "drawColor", "(I)V"), kBg);
    jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
    env->CallVoidMethod(paint, env->GetMethodID(paintC, "setColor", "(I)V"), kFg);
    env->CallVoidMethod(canvas,
                        env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V"),
                        80.0f, 60.0f, 400.0f, 260.0f, paint);
    if (env->ExceptionCheck()) return jfail("draw");

    // ── Read the rendered pixels → gfx::Surface ─────────────────────────────
    const jlong handle = env->CallLongMethod(
        bitmap, env->GetMethodID(bitmapC, "getNativeInstance", "()J"));
    auto read = reinterpret_cast<bool (*)(long, std::uint32_t*, int, int)>(
        ::dlsym(RTLD_DEFAULT, "acl_read_bitmap"));
    if (read == nullptr) { log::w(kTag, "graphics demo: acl_read_bitmap missing"); return; }
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(W) * H);
    if (!read(handle, pixels.data(), W, H)) {
      log::w(kTag, "graphics demo: acl_read_bitmap failed");
      return;
    }
    gfx::Surface surf(W, H);
    surf.blit(pixels.data());

    // ── Import to dma-buf + present to the compositor ───────────────────────
    std::string err;
    auto rt = gfx::VkRenderTarget::Create(W, H, &err);
    if (!rt) { log::w(kTag, "graphics demo: no Vulkan render target: {}", err); return; }
    wm::DmabufRenderFn render = [&](const std::vector<std::uint64_t>&,
                                    gfx::DmabufFrame* out) {
      return rt->import_pixels_to_dmabuf(surf, out, &err);
    };
    const bool ok = wm::present_dmabuf(render, "acl graphics", "com.acl.graphics", 3);
    log::i(kTag, "graphics demo: app→android.graphics→Android Skia→dma-buf→Wayland {}",
           ok ? "presented" : "did not present (no compositor?)");
#endif
  }

  // Pre-load the framework's system assets (framework-res.apk = the android:R.* resources)
  // so Resources.getSystem() resolves real framework resources. AssetManager hardcodes
  // FRAMEWORK_APK_PATH=/system/framework/framework-res.apk with no override, but its
  // createSystemAssetsInZygoteLocked(reinit, path) is public and sets sSystem — so acl
  // pre-populates from a real path (ACL_ART_FRAMEWORK_RES) before getSystem() runs.
  void load_system_resources(JNIEnv* env) {
    const char* res = std::getenv("ACL_ART_FRAMEWORK_RES");
    if (res == nullptr || *res == '\0') return;
    jclass am = env->FindClass("android/content/res/AssetManager");
    if (am == nullptr) { env->ExceptionClear(); return; }
    jmethodID create = env->GetStaticMethodID(
        am, "createSystemAssetsInZygoteLocked", "(ZLjava/lang/String;)V");
    if (create == nullptr) { env->ExceptionClear(); return; }
    jstring path = env->NewStringUTF(res);
    env->CallStaticVoidMethod(am, create, JNI_FALSE, path);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      log::w(kTag, "ART framework resources: failed to load {}", res);
    } else {
      log::i(kTag, "ART framework resources: loaded {}", res);
    }
    env->DeleteLocalRef(path);
    env->DeleteLocalRef(am);
  }

  // Populate a minimal build.prop before the app's framework classes initialize. Many
  // framework static initializers read system properties and crash on empty values
  // (e.g. android.os.Build.<clinit> indexes ro.product.cpu.abilist* — a length-0 array
  // without these). On host, properties are an in-process map that SystemProperties.set
  // writes, so acl seeds the essential set. Done after the SystemProperties native is
  // registered above; values are placeholders, not a claim of a real device.
  void populate_system_properties(JNIEnv* env) {
    jclass sp = env->FindClass("android/os/SystemProperties");
    if (sp == nullptr) { env->ExceptionClear(); return; }
    jmethodID set = env->GetStaticMethodID(
        sp, "set", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (set == nullptr) { env->ExceptionClear(); return; }
    struct Prop { const char* k; const char* v; };
    static const Prop kProps[] = {
        {"ro.product.cpu.abilist", "x86_64,x86"}, {"ro.product.cpu.abilist64", "x86_64"},
        {"ro.product.cpu.abilist32", "x86"},
        {"ro.build.version.sdk", "33"}, {"ro.build.version.release", "13"},
        {"ro.build.version.codename", "REL"}, {"ro.build.version.incremental", "acl"},
        {"ro.build.version.all_codenames", "REL"}, {"ro.build.version.known_codenames", "REL"},
        {"ro.build.version.preview_sdk", "0"}, {"ro.build.version.preview_sdk_fingerprint", "REL"},
        {"ro.build.version.min_supported_target_sdk", "0"}, {"ro.build.version.security_patch", "2022-01-01"},
        {"ro.build.id", "acl"}, {"ro.build.display.id", "acl"}, {"ro.build.type", "eng"},
        {"ro.build.tags", "test-keys"}, {"ro.build.flavor", "acl-eng"},
        {"ro.build.date.utc", "0"}, {"ro.build.host", "acl"}, {"ro.build.user", "acl"},
        {"ro.build.fingerprint", "acl/acl/acl:13/acl/acl:eng/test-keys"},
        {"ro.product.name", "acl"}, {"ro.product.device", "acl"},
        {"ro.product.board", "acl"}, {"ro.product.brand", "acl"},
        {"ro.product.model", "acl"}, {"ro.product.manufacturer", "acl"},
        {"ro.hardware", "acl"}, {"ro.bootloader", "acl"}, {"ro.secure", "0"},
        {"ro.debuggable", "1"}, {"ro.dalvik.vm.native.bridge", "0"},
    };
    int n = 0;
    for (const Prop& p : kProps) {
      jstring k = env->NewStringUTF(p.k);
      jstring v = env->NewStringUTF(p.v);
      env->CallStaticVoidMethod(sp, set, k, v);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      } else {
        ++n;
      }
      env->DeleteLocalRef(k);
      env->DeleteLocalRef(v);
    }
    env->DeleteLocalRef(sp);
    log::i(kTag, "ART build.prop: seeded {} system propert{}", n, n == 1 ? "y" : "ies");
  }
};

}  // namespace

std::unique_ptr<Vm> make_jvm_vm(VmConfig config) {
  return std::make_unique<JvmVm>(std::move(config));
}

std::unique_ptr<Vm> make_art_vm(VmConfig config) {
  return std::make_unique<ArtVm>(std::move(config));
}

}  // namespace acl::runtime

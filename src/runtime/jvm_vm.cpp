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

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <cstdlib>
#include <ctime>

#include <dlfcn.h>
#include <jni.h>

#include "acl_config.h"
#include "audio/audio.h"
#include "gfx/gfx.h"
#include "log/log.h"
#include "runtime/framework_natives.h"
#include "svc/svc.h"
#include "wm/present.h"
#if ACL_PROFILE_AUTOMOTIVE
#include "hosts/cal/cal.h"             // Gate G1.5b: Car App Library template host
#include "hosts/media/media_bridge.h"  // Gate G1.5a: headless media bridge (automotive)
#endif

namespace acl::runtime {
namespace {

constexpr std::string_view kTag = "acl.runtime.jvm";

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

    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject activity = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (activity == nullptr) {
      result = fail_with_exception("instantiate " + std::string(activity_class));
    } else {
      log::i(kTag, "launching Activity {}", activity_class);
      log_context(activity);
      // onCreate(null) -> onStart() -> onResume(), the documented launch order.
      result = call_lifecycle(cls, activity, "onCreate", "(Landroid/os/Bundle;)V", true);
      if (result.is_ok()) probe_managers(activity);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onStart", "()V", false);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onResume", "()V", false);
      if (result.is_ok()) render_content_view(activity);
      env_->DeleteLocalRef(activity);
    }
    env_->DeleteLocalRef(activity_cls);
    env_->DeleteLocalRef(cls);
    return result;
  }

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
  void render_content_view(jobject activity) {
    jclass acls = env_->GetObjectClass(activity);
    jmethodID gcv = env_->GetMethodID(acls, "aclContentView", "()Landroid/view/View;");
    env_->DeleteLocalRef(acls);
    if (gcv == nullptr) { clear_exception(); return; }
    jobject view = env_->CallObjectMethod(activity, gcv);
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
      jmethodID create = env_->GetStaticMethodID(bmp_cls, "createBitmap",
                                                 "(II)Landroid/graphics/Bitmap;");
      jmethodID cctor = env_->GetMethodID(canvas_cls, "<init>",
                                          "(Landroid/graphics/Bitmap;)V");
      if (create != nullptr && cctor != nullptr) {
        bmp = env_->CallStaticObjectMethod(bmp_cls, create, w, h);
        jobject canvas = bmp != nullptr ? env_->NewObject(canvas_cls, cctor, bmp) : nullptr;
        if (canvas != nullptr) {
          env_->CallVoidMethod(view, draw, canvas);
          jmethodID describe = env_->GetMethodID(bmp_cls, "aclDescribe", "()V");
          if (describe != nullptr) env_->CallVoidMethod(bmp, describe);
          // Present the content view in a Wayland window (--present), driving a
          // Choreographer loop: each vsync tick fires the app's frame callbacks and
          // re-draws. No-op if no compositor / built without wcs.
          if (config_.present) present_content_loop(view, bmp, canvas, w, h);
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
    if (ptr_m == nullptr) { clear_exception(); return; }
    auto* surface = reinterpret_cast<const gfx::Surface*>(env_->CallLongMethod(bmp, ptr_m));
    if (surface == nullptr) return;

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
      env_->ExceptionDescribe();
      env_->ExceptionClear();
    }
    return Error::of(Status::RuntimeError, "failed to " + what);
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

    const jint rc =
        JNI_CreateJavaVM(&jvm_, reinterpret_cast<void**>(&env), &args);
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
    } else {
      option_strings.push_back("-Xint");  // imageless: interpret; no runtime dex2oat
    }
    option_strings.push_back("-Djava.class.path=" + join(cp, ':'));
    for (const std::string& opt : config_.options) option_strings.push_back(opt);

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
    populate_system_properties(env);
    load_system_resources(env);
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

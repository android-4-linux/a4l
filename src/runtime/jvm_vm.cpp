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
#include <vector>

#include <jni.h>

#include "gfx/gfx.h"
#include "log/log.h"
#include "runtime/framework_natives.h"
#include "svc/svc.h"
#include "wm/present.h"

namespace aclcxx::runtime {
namespace {

constexpr std::string_view kTag = "aclcxx.runtime.jvm";

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

class JvmVm final : public Vm {
 public:
  explicit JvmVm(VmConfig config) : Vm(Backend::Jvm), config_(std::move(config)) {}

  ~JvmVm() override { shutdown(); }

  Error start() override {
    if (jvm_ != nullptr) return Error::ok();

    // Assemble VM options. A stock JVM has no separate boot classpath we can
    // safely repopulate, so the vendored framework (boot_classpath) is prepended
    // to java.class.path: it resolves ahead of, and independently of, app classes.
    // The ART target (R-1) will instead build a real boot image from these entries.
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

    JNIEnv* env = nullptr;
    const jint rc =
        JNI_CreateJavaVM(&jvm_, reinterpret_cast<void**>(&env), &args);
    if (rc != JNI_OK || jvm_ == nullptr) {
      jvm_ = nullptr;
      return Error::of(Status::RuntimeError,
                       "JNI_CreateJavaVM failed (rc=" + std::to_string(rc) + ")");
    }
    env_ = env;
    log::i(kTag, "JVM started: boot=[{}] classpath=[{}]",
           join(config_.boot_classpath, ':'), join(config_.classpath, ':'));
    // Bind aclcxx C++ to the android.* native methods on the boot classpath.
    register_framework_natives(env_);
    return Error::ok();
  }

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
    if (cls == nullptr) {
      clear_exception();
      return Error::of(Status::NotFound, "class not found: " + std::string(activity_class));
    }
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

  Error call_media_browser(std::string_view service_class) override {
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_media_browser before start()");
    }
    const std::string name = to_jni_name(service_class);
    jclass cls = env_->FindClass(name.c_str());
    if (cls == nullptr) {
      clear_exception();
      return Error::of(Status::NotFound, "class not found: " + std::string(service_class));
    }
    jclass mbs_cls = env_->FindClass("android/service/media/MediaBrowserService");
    if (mbs_cls == nullptr || env_->IsAssignableFrom(cls, mbs_cls) != JNI_TRUE) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      if (mbs_cls != nullptr) env_->DeleteLocalRef(mbs_cls);
      return Error::of(Status::NotApplicable,
                       std::string(service_class) + " is not a MediaBrowserService");
    }

    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject svc = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (svc == nullptr) {
      result = fail_with_exception("instantiate " + std::string(service_class));
    } else {
      log::i(kTag, "hosting MediaBrowserService {}", service_class);
      log_context(svc);
      result = call_lifecycle(cls, svc, "onCreate", "()V", false);
      if (result.is_ok()) probe_managers(svc);  // the app's MediaSession now shows up
      if (result.is_ok()) result = read_session(cls, svc);
      if (result.is_ok()) result = browse_root(cls, svc);
      env_->DeleteLocalRef(svc);
    }
    env_->DeleteLocalRef(mbs_cls);
    env_->DeleteLocalRef(cls);
    return result;
  }

  Error shutdown() override {
    if (jvm_ != nullptr) {
      jvm_->DestroyJavaVM();  // blocks until non-daemon threads finish
      jvm_ = nullptr;
      env_ = nullptr;
      log::i(kTag, "JVM shutdown");
    }
    return Error::ok();
  }

 private:
  void clear_exception() {
    if (env_ != nullptr && env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionClear();
    }
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
            env_->GetMethodID(obj_cls, "aclcxxSetPackage", "(Ljava/lang/String;)V");
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
             call_int(msm, "aclcxxActiveSessionCount"));
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
    jmethodID gcv = env_->GetMethodID(acls, "aclcxxContentView", "()Landroid/view/View;");
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
          jmethodID describe = env_->GetMethodID(bmp_cls, "aclcxxDescribe", "()V");
          if (describe != nullptr) env_->CallVoidMethod(bmp, describe);
          // Present the rendered frame in a Wayland window (--present). The Bitmap's
          // native pointer IS the gfx::Surface just drawn; hand it to wm/ (wcs). No-op
          // if no compositor is reachable or aclcxx was built without wcs.
          if (config_.present) present_content(bmp_cls, bmp);
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

  // Present the rendered Bitmap (a gfx::Surface behind aclcxxPtr()) in a Wayland
  // window via wm/ (wcs). One-shot: blocks until the frame is shown or times out.
  void present_content(jclass bmp_cls, jobject bmp) {
    jmethodID ptr_m = env_->GetMethodID(bmp_cls, "aclcxxPtr", "()J");
    if (ptr_m == nullptr) { clear_exception(); return; }
    const jlong ptr = env_->CallLongMethod(bmp, ptr_m);
    auto* surface = reinterpret_cast<const gfx::Surface*>(ptr);
    if (surface == nullptr) return;
    if (!wm::wayland_available()) {
      log::i(kTag, "--present: no Wayland compositor reachable; frame not shown");
      return;
    }
    const std::string& pkg = config_.package_name;
    wm::present_surface(*surface, pkg.empty() ? "aclcxx" : pkg,
                        pkg.empty() ? "org.aclcxx.app" : pkg);
  }

  // Print + clear a pending exception and return a RuntimeError describing `what`.
  Error fail_with_exception(const std::string& what) {
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();
      env_->ExceptionClear();
    }
    return Error::of(Status::RuntimeError, "failed to " + what);
  }

  // Call MediaBrowserService.onGetRoot(pkg, uid, null) and read BrowserRoot.getRootId().
  Error browse_root(jclass cls, jobject svc) {
    jmethodID on_get_root = env_->GetMethodID(
        cls, "onGetRoot",
        "(Ljava/lang/String;ILandroid/os/Bundle;)"
        "Landroid/service/media/MediaBrowserService$BrowserRoot;");
    if (on_get_root == nullptr) {
      clear_exception();
      return Error::of(Status::NotFound, "no onGetRoot(String,int,Bundle)");
    }
    jstring pkg = env_->NewStringUTF("aclcxx");
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
      result = load_children(cls, svc, id);  // walk the root's children
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

  Error load_children(jclass cls, jobject svc, jstring parent_id) {
    jmethodID on_load = env_->GetMethodID(
        cls, "onLoadChildren",
        "(Ljava/lang/String;Landroid/service/media/MediaBrowserService$Result;)V");
    if (on_load == nullptr) { clear_exception(); return Error::ok(); }  // optional

    jclass result_cls = env_->FindClass("android/service/media/MediaBrowserService$Result");
    jmethodID result_ctor =
        result_cls != nullptr ? env_->GetMethodID(result_cls, "<init>", "()V") : nullptr;
    jobject result = result_ctor != nullptr ? env_->NewObject(result_cls, result_ctor) : nullptr;
    if (result == nullptr) { clear_exception(); return fail_with_exception("build Result"); }

    env_->CallVoidMethod(svc, on_load, parent_id, result);
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->DeleteLocalRef(result); env_->DeleteLocalRef(result_cls);
      return fail_with_exception("onLoadChildren()");
    }

    // list = result.aclcxxValue()
    jobject list = env_->CallObjectMethod(
        result, env_->GetMethodID(result_cls, "aclcxxValue", "()Ljava/lang/Object;"));
    Error result_err = Error::ok();
    if (list == nullptr) {
      log::i(kTag, "media children: (none sent)");
    } else {
      jclass list_cls = env_->FindClass("java/util/List");
      jmethodID size_m = env_->GetMethodID(list_cls, "size", "()I");
      jmethodID get_m = env_->GetMethodID(list_cls, "get", "(I)Ljava/lang/Object;");
      const jint n = env_->CallIntMethod(list, size_m);
      log::i(kTag, "media children of '{}': {} item(s)", utf8(parent_id), n);
      for (jint i = 0; i < n; ++i) {
        jobject item = env_->CallObjectMethod(list, get_m, i);  // MediaBrowser.MediaItem
        jclass item_cls = env_->GetObjectClass(item);
        jobject desc = env_->CallObjectMethod(
            item, env_->GetMethodID(item_cls, "getDescription",
                                    "()Landroid/media/MediaDescription;"));
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
        env_->DeleteLocalRef(desc_cls); env_->DeleteLocalRef(desc);
        env_->DeleteLocalRef(item_cls); env_->DeleteLocalRef(item);
      }
      env_->DeleteLocalRef(list_cls);
      env_->DeleteLocalRef(list);
    }
    env_->DeleteLocalRef(result);
    env_->DeleteLocalRef(result_cls);
    return result_err;
  }

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
        token, env_->GetMethodID(token_cls, "aclcxxSession",
                                 "()Landroid/media/session/MediaSession;"));
    jclass ses_cls = session != nullptr ? env_->GetObjectClass(session) : nullptr;

    std::string title = "(none)";
    int state = -1;
    if (session != nullptr) {
      jobject md = env_->CallObjectMethod(
          session, env_->GetMethodID(ses_cls, "aclcxxMetadata",
                                     "()Landroid/media/MediaMetadata;"));
      if (md != nullptr) {
        jclass md_cls = env_->GetObjectClass(md);
        jstring key = env_->NewStringUTF("android.media.metadata.TITLE");
        auto t = static_cast<jstring>(env_->CallObjectMethod(
            md, env_->GetMethodID(md_cls, "getString",
                                  "(Ljava/lang/String;)Ljava/lang/String;"), key));
        if (t != nullptr) { title = utf8(t); env_->DeleteLocalRef(t); }
        env_->DeleteLocalRef(key); env_->DeleteLocalRef(md_cls); env_->DeleteLocalRef(md);
      }
      jobject ps = env_->CallObjectMethod(
          session, env_->GetMethodID(ses_cls, "aclcxxPlaybackState",
                                     "()Landroid/media/session/PlaybackState;"));
      if (ps != nullptr) {
        jclass ps_cls = env_->GetObjectClass(ps);
        state = env_->CallIntMethod(ps, env_->GetMethodID(ps_cls, "getState", "()I"));
        env_->DeleteLocalRef(ps_cls); env_->DeleteLocalRef(ps);
      }
    }
    log::i(kTag, "media session: title='{}' state={}", title, state);
    if (session != nullptr) invoke_transport(ses_cls, session);
    if (ses_cls != nullptr) env_->DeleteLocalRef(ses_cls);
    if (session != nullptr) env_->DeleteLocalRef(session);
    env_->DeleteLocalRef(token_cls);
    env_->DeleteLocalRef(token);
    return Error::ok();
  }

  // Drive transport controls: fetch the session's Callback and invoke onPlay()
  // then onPause() in-process (no controller/Binder) — the app's overrides react.
  void invoke_transport(jclass ses_cls, jobject session) {
    jobject cb = env_->CallObjectMethod(
        session, env_->GetMethodID(ses_cls, "aclcxxCallback",
                                   "()Landroid/media/session/MediaSession$Callback;"));
    if (cb == nullptr) { clear_exception(); return; }  // app set no callback
    jclass cb_cls = env_->GetObjectClass(cb);
    for (const char* action : {"onPlay", "onPause"}) {
      jmethodID m = env_->GetMethodID(cb_cls, action, "()V");
      if (m == nullptr) { clear_exception(); continue; }
      log::i(kTag, "media transport: {}", action);
      env_->CallVoidMethod(cb, m);
      if (env_->ExceptionCheck() == JNI_TRUE) {
        env_->ExceptionDescribe();
        env_->ExceptionClear();
      }
    }
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

  VmConfig config_;
  JavaVM* jvm_ = nullptr;
  JNIEnv* env_ = nullptr;
};

}  // namespace

std::unique_ptr<Vm> make_jvm_vm(VmConfig config) {
  return std::make_unique<JvmVm>(std::move(config));
}

}  // namespace aclcxx::runtime

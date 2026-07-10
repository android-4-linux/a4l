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

#include "log/log.h"
#include "runtime/framework_natives.h"

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
      // onCreate(null) -> onStart() -> onResume(), the documented launch order.
      result = call_lifecycle(cls, activity, "onCreate", "(Landroid/os/Bundle;)V", true);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onStart", "()V", false);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onResume", "()V", false);
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
      result = call_lifecycle(cls, svc, "onCreate", "()V", false);
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
      if (id != nullptr) env_->DeleteLocalRef(id);
    }
    env_->DeleteLocalRef(root_cls);
    env_->DeleteLocalRef(root);
    return result;
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

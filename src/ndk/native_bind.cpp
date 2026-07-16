// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl. Name mangling per the public JNI spec (Oracle JNI 6, "Resolving Native
// Method Names"); native-method discovery via public java.lang.Class/java.lang.reflect.Method
// reflection (Android SDK docs). ART binding contract read from AOSP art/runtime/jni/java_vm_ext.cc
// (Apache-2.0). No GPL-3.0 sources.

#include "ndk/native_bind.h"

#if ACL_HAVE_JNI
#include <cstdio>
#include <string>
#include <string_view>

#include "log/log.h"
#include "ndk/bionic_runtime.h"

namespace acl::ndk {
namespace {

constexpr std::string_view kTag = "acl.ndk";
constexpr jint kAccNative = 0x0100;

// JNI spec mangling: '.'/'/' -> '_', '_' -> "_1", ';' -> "_2", '[' -> "_3", and any other
// non-alphanumeric -> "_0xxxx" (the UTF-16 unit). Java identifiers here are ASCII in practice; a
// non-ASCII one would need real UTF-8 -> UTF-16 decoding before the escape, so it is rejected
// (returns false) rather than mangled wrongly — a wrong name would silently fail to bind.
bool mangle(std::string_view s, std::string& out) {
  for (const char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    else if (c == '.' || c == '/') out += '_';
    else if (c == '_') out += "_1";
    else if (c == ';') out += "_2";
    else if (c == '[') out += "_3";
    else if (static_cast<unsigned char>(c) < 0x80) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "_0%04x", static_cast<unsigned char>(c));
      out += buf;
    } else {
      return false;  // non-ASCII identifier: would need UTF-16 escapes; don't guess
    }
  }
  return true;
}

std::string jstring_to_utf8(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

// A JVM type descriptor for the java.lang.Class `type`, via Class.getName(), whose format is
// "int" | "[I" | "java.lang.String" | "[Ljava.lang.String;" (arrays already come back in
// descriptor form, just dot-separated).
std::string descriptor_of(JNIEnv* env, jclass class_cls, jmethodID get_name, jobject type) {
  static constexpr struct {
    const char* name;
    const char* desc;
  } kPrimitives[] = {{"int", "I"},   {"boolean", "Z"}, {"byte", "B"},  {"char", "C"},
                     {"short", "S"}, {"long", "J"},    {"float", "F"}, {"double", "D"},
                     {"void", "V"}};
  (void)class_cls;
  auto name_str = reinterpret_cast<jstring>(env->CallObjectMethod(type, get_name));
  std::string name = jstring_to_utf8(env, name_str);
  if (name_str) env->DeleteLocalRef(name_str);
  for (const auto& p : kPrimitives)
    if (name == p.name) return p.desc;
  for (char& c : name)
    if (c == '.') c = '/';
  if (!name.empty() && name[0] == '[') return name;  // already a descriptor
  return "L" + name + ";";
}

}  // namespace

jclass loading_class(JNIEnv* env, jobject caller, jobject loader) {
  if (caller != nullptr) return reinterpret_cast<jclass>(env->NewLocalRef(caller));

  // Walk the Java stack for the frame that called System.loadLibrary. At this point the stack is
  //   java.lang.Runtime.nativeLoad  <- us
  //   java.lang.Runtime.loadLibrary0
  //   java.lang.System.loadLibrary
  //   com.acl.ndktest.Main.<clinit> <- the first non-java.lang/dalvik.system frame: what we want
  auto give_up = [&](const char* why) -> jclass {
    if (env->ExceptionCheck()) env->ExceptionClear();
    log::w(kTag, "bind-by-name: cannot identify the loading class ({})", why);
    return nullptr;
  };
  jclass thr_cls = env->FindClass("java/lang/Throwable");
  jclass ste_cls = env->FindClass("java/lang/StackTraceElement");
  if (thr_cls == nullptr || ste_cls == nullptr) return give_up("Throwable/StackTraceElement");
  jmethodID thr_init = env->GetMethodID(thr_cls, "<init>", "()V");
  jmethodID get_trace =
      env->GetMethodID(thr_cls, "getStackTrace", "()[Ljava/lang/StackTraceElement;");
  jmethodID ste_class_name = env->GetMethodID(ste_cls, "getClassName", "()Ljava/lang/String;");
  if (thr_init == nullptr || get_trace == nullptr || ste_class_name == nullptr)
    return give_up("methodIDs");
  jobject thr = env->NewObject(thr_cls, thr_init);
  if (thr == nullptr) return give_up("new Throwable");
  auto frames = reinterpret_cast<jobjectArray>(env->CallObjectMethod(thr, get_trace));
  env->DeleteLocalRef(thr);
  if (frames == nullptr) return give_up("getStackTrace");

  std::string found;
  const jsize n = env->GetArrayLength(frames);
  for (jsize i = 0; i < n && found.empty(); ++i) {
    jobject f = env->GetObjectArrayElement(frames, i);
    if (f == nullptr) continue;
    auto s = reinterpret_cast<jstring>(env->CallObjectMethod(f, ste_class_name));
    const std::string name = jstring_to_utf8(env, s);
    if (s) env->DeleteLocalRef(s);
    env->DeleteLocalRef(f);
    if (name.rfind("java.lang.", 0) == 0 || name.rfind("dalvik.system.", 0) == 0) continue;
    found = name;
  }
  env->DeleteLocalRef(frames);
  if (found.empty()) return give_up("no app frame on the stack");

  // Resolve through the ClassLoader we were handed. initialize=false: `found` is mid-<clinit>
  // (its static initializer is what called loadLibrary), and we only need its declared methods.
  jclass cls_cls = env->FindClass("java/lang/Class");
  jmethodID for_name = env->GetStaticMethodID(
      cls_cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
  if (for_name == nullptr) return give_up("Class.forName");
  jstring jname = env->NewStringUTF(found.c_str());
  auto k = reinterpret_cast<jclass>(
      env->CallStaticObjectMethod(cls_cls, for_name, jname, JNI_FALSE, loader));
  env->DeleteLocalRef(jname);
  if (k == nullptr || env->ExceptionCheck()) return give_up(found.c_str());
  return k;
}

int bind_class_natives(BionicRuntime& rt, void* handle, JNIEnv* env, jclass klass) {
  if (handle == nullptr || klass == nullptr) return 0;
  auto fail = [&](const char* what) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    log::w(kTag, "bind-by-name: reflection failed ({})", what);
    return -1;
  };

  jclass class_cls = env->FindClass("java/lang/Class");
  jclass method_cls = env->FindClass("java/lang/reflect/Method");
  if (class_cls == nullptr || method_cls == nullptr) return fail("Class/Method");
  jmethodID cls_get_name = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
  jmethodID cls_get_methods =
      env->GetMethodID(class_cls, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
  jmethodID m_get_name = env->GetMethodID(method_cls, "getName", "()Ljava/lang/String;");
  jmethodID m_get_mods = env->GetMethodID(method_cls, "getModifiers", "()I");
  jmethodID m_get_params = env->GetMethodID(method_cls, "getParameterTypes", "()[Ljava/lang/Class;");
  jmethodID m_get_ret = env->GetMethodID(method_cls, "getReturnType", "()Ljava/lang/Class;");
  if (cls_get_name == nullptr || cls_get_methods == nullptr || m_get_name == nullptr ||
      m_get_mods == nullptr || m_get_params == nullptr || m_get_ret == nullptr)
    return fail("methodIDs");

  auto cls_name_str = reinterpret_cast<jstring>(env->CallObjectMethod(klass, cls_get_name));
  const std::string cls_name = jstring_to_utf8(env, cls_name_str);  // binary name: com.acl.X
  if (cls_name_str) env->DeleteLocalRef(cls_name_str);
  std::string mangled_cls;
  if (cls_name.empty() || !mangle(cls_name, mangled_cls)) return fail("class name");

  auto methods = reinterpret_cast<jobjectArray>(env->CallObjectMethod(klass, cls_get_methods));
  if (methods == nullptr) return fail("getDeclaredMethods");

  int bound = 0;
  const jsize n = env->GetArrayLength(methods);
  for (jsize i = 0; i < n; ++i) {
    jobject m = env->GetObjectArrayElement(methods, i);
    if (m == nullptr) continue;
    if ((env->CallIntMethod(m, m_get_mods) & kAccNative) == 0) {
      env->DeleteLocalRef(m);
      continue;
    }
    auto m_name_str = reinterpret_cast<jstring>(env->CallObjectMethod(m, m_get_name));
    const std::string m_name = jstring_to_utf8(env, m_name_str);
    if (m_name_str) env->DeleteLocalRef(m_name_str);

    // Signature: "(" + each parameter descriptor + ")" + the return descriptor.
    auto params = reinterpret_cast<jobjectArray>(env->CallObjectMethod(m, m_get_params));
    std::string args;
    if (params != nullptr) {
      const jsize np = env->GetArrayLength(params);
      for (jsize p = 0; p < np; ++p) {
        jobject t = env->GetObjectArrayElement(params, p);
        args += descriptor_of(env, class_cls, cls_get_name, t);
        env->DeleteLocalRef(t);
      }
      env->DeleteLocalRef(params);
    }
    jobject ret = env->CallObjectMethod(m, m_get_ret);
    const std::string sig = "(" + args + ")" + descriptor_of(env, class_cls, cls_get_name, ret);
    if (ret) env->DeleteLocalRef(ret);
    env->DeleteLocalRef(m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }

    // A JNI_OnLoad binding is the app's explicit choice; never overwrite it by name.
    if (rt.was_registered(env, klass, m_name.c_str(), sig.c_str())) continue;

    // Short name first; the long name (…__ + mangled arg descriptors) is what the spec requires
    // for an OVERLOADED native, so a .so with two natives of the same name exports only long ones.
    std::string sym = "Java_" + mangled_cls + "_";
    std::string mangled_m;
    if (m_name.empty() || !mangle(m_name, mangled_m)) continue;
    sym += mangled_m;
    void* tramp = rt.downcall_symbol(handle, sym.c_str());
    if (tramp == nullptr) {
      std::string mangled_args;
      if (!mangle(args, mangled_args)) continue;
      const std::string long_sym = sym + "__" + mangled_args;
      tramp = rt.downcall_symbol(handle, long_sym.c_str());
    }
    if (tramp == nullptr) continue;  // not exported by name (e.g. bound via JNI_OnLoad, or absent)

    JNINativeMethod reg{const_cast<char*>(m_name.c_str()), const_cast<char*>(sig.c_str()), tramp};
    if (env->RegisterNatives(klass, &reg, 1) == JNI_OK) {
      ++bound;
      log::i(kTag, "bind-by-name: {}.{}{} -> {}", cls_name, m_name, sig, sym);
    } else if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
  }
  env->DeleteLocalRef(methods);
  return bound;
}

}  // namespace acl::ndk
#endif  // ACL_HAVE_JNI

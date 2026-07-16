// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl. Mirrors the other src/**/ *_probe mains. Exercises the in-process
// bionic loader (src/ndk/bionic_runtime) against a bionic .so. No GPL-3.0 sources.
//
// Usage: acl-ndk-probe <target.so> <symbol>
//   env: ACL_BIONIC_LINKER, ACL_BIONIC_SYSROOT, ACL_BIONIC_STUB (see docs/ndk-scope.md).
// Loads the bionic .so in-process on the glibc host, resolves <symbol>, and calls it as an
// int(JNIEnv*,jclass) (the tools/ndk-loader-spike repro's nativeAnswer ignores both args).
// Prints the returned int; exit 0 iff it returns 42 (the repro's value).

#include <cerrno>
#include <cstdio>
#include <string>

#include "ndk/bionic_runtime.h"

// A glibc-side callback for the boundary self-test: writes glibc TLS (errno) and doubles x. If the
// up-call trampoline failed to restore glibc %fs, this errno write would corrupt bionic's TLS.
static int glibc_cb(int x) {
  errno = 99;
  return x * 2;
}

#if ACL_HAVE_JNI
// A minimal FAKE "ART" JavaVM/JNIEnv living on the glibc side. Each function writes glibc TLS
// (probe_tls) — if a trampoline failed to restore glibc %fs, that write would land in bionic's TLS
// and the values below would not survive. RegisterNatives captures the bound function pointer.
namespace {
const void* g_captured_native = nullptr;
int g_glibc_calls = 0;
__thread int probe_tls;  // glibc TLS, exercised during the up-calls

jint JNICALL fake_GetVersion(JNIEnv*) { probe_tls = 111; ++g_glibc_calls; return JNI_VERSION_1_6; }
jclass JNICALL fake_FindClass(JNIEnv*, const char*) {
  probe_tls = 222; ++g_glibc_calls; return reinterpret_cast<jclass>(0x1234);
}
jint JNICALL fake_RegisterNatives(JNIEnv*, jclass, const JNINativeMethod* m, jint n) {
  probe_tls = 333; ++g_glibc_calls;
  if (n >= 1) g_captured_native = m[0].fnPtr;
  return JNI_OK;
}
// The wrapped RegisterNatives records each binding (so name resolution won't overwrite it), which
// needs NewGlobalRef. No GC here, so handing back the same ref is a faithful enough fake.
// Deliberately NOT counted in g_glibc_calls: that counter means "up-calls the .so made", and this
// is acl's own bookkeeping inside the RegisterNatives thunk, not a call the .so asked for.
jobject JNICALL fake_NewGlobalRef(JNIEnv*, jobject o) { return o; }
jmethodID JNICALL fake_GetStaticMethodID(JNIEnv*, jclass, const char*, const char*) {
  ++g_glibc_calls; return reinterpret_cast<jmethodID>(0x2);
}
jint JNICALL fake_CallStaticIntMethodV(JNIEnv*, jclass, jmethodID, va_list a) {
  probe_tls = 444; ++g_glibc_calls;
  int x = va_arg(a, int), y = va_arg(a, int);  // pulls the varargs forwarded from w_CallStaticIntMethod
  return x + y;
}
acl::ndk::JniEnvTable g_fake_env_tbl;
struct FakeEnv { const acl::ndk::JniEnvTable* functions; } g_fake_env;

jint JNICALL fake_GetEnv(JavaVM*, void** out, jint) {
  ++g_glibc_calls;
  *out = &g_fake_env;
  return JNI_OK;
}
acl::ndk::JniVmTable g_fake_vm_tbl;
struct FakeVM { const acl::ndk::JniVmTable* functions; } g_fake_vm;
}  // namespace

static bool run_jni_selftest(acl::ndk::BionicRuntime& rt, const char* so_path) {
  // Build the fake glibc "ART".
  g_fake_env_tbl = {};
  g_fake_env_tbl.GetVersion = fake_GetVersion;
  g_fake_env_tbl.FindClass = fake_FindClass;
  g_fake_env_tbl.RegisterNatives = fake_RegisterNatives;
  g_fake_env_tbl.NewGlobalRef = fake_NewGlobalRef;
  g_fake_env_tbl.GetStaticMethodID = fake_GetStaticMethodID;
  g_fake_env_tbl.CallStaticIntMethodV = fake_CallStaticIntMethodV;
  g_fake_env.functions = &g_fake_env_tbl;
  g_fake_vm_tbl = {};
  g_fake_vm_tbl.GetEnv = fake_GetEnv;
  g_fake_vm.functions = &g_fake_vm_tbl;

  JNIEnv* real_env = reinterpret_cast<JNIEnv*>(&g_fake_env);
  JavaVM* real_vm = reinterpret_cast<JavaVM*>(&g_fake_vm);

  probe_tls = 0;
  g_glibc_calls = 0;
  g_captured_native = nullptr;

  // The exact operation Runtime.nativeLoad will call: load + drive JNI_OnLoad across the boundary.
  std::string err;
  if (!rt.load_library(so_path, real_vm, real_env, &err)) {
    std::fprintf(stderr, "load_library failed: %s\n", err.c_str());
    return false;
  }
  const jint ver = JNI_VERSION_1_6;  // load_library validated JNI_OnLoad's requested version

  // Invoke the registered native exactly as ART would: from GLIBC %fs, and what RegisterNatives
  // handed ART is the down-call TRAMPOLINE (not the raw bionic native). The trampoline swaps to
  // bionic %fs and substitutes the wrapped env; the native's CallIntMethod up-call trampolines back
  // to glibc. Round-trip: 4200 + (20+30) = 4250. (No RunScope here — glibc %fs, like ART.)
  jint native_ret = -1;
  if (g_captured_native) {
    native_ret = reinterpret_cast<jint (*)(JNIEnv*, jclass)>(
        const_cast<void*>(g_captured_native))(real_env, nullptr);
  }

  // 6 up-calls: GetEnv, GetVersion, FindClass, RegisterNatives (JNI_OnLoad) + GetStaticMethodID,
  // CallStaticIntMethod (native).
  const bool ok = ver == JNI_VERSION_1_6 && g_captured_native != nullptr && g_glibc_calls == 6 &&
                  probe_tls == 444 && native_ret == 4250;
  std::printf(
      "acl-ndk-probe: JNI_OnLoad ver=%#x up-calls=%d probe_tls=%d native=%d (varargs up-call) -> %s\n",
      ver, g_glibc_calls, probe_tls, native_ret, ok ? "(PASS)" : "(FAIL)");
  return ok;
}

// End-to-end against a REAL JVM (the host OpenJDK linked via jni_dep) — the strongest proof that the
// jvm_vm.cpp Runtime.nativeLoad hook will work: boot a genuine JVM, load_library the bionic .so
// (drives JNI_OnLoad -> FindClass(Main) -> RegisterNatives(answer) with a down-call trampoline), then
// call Main.answer() from Java. The JVM's own dispatch invokes the trampoline -> bionic native ->
// GetStaticMethodID/CallStaticIntMethod(helper,20,30) up-calls -> back. Expect 4250.
static bool run_realvm(acl::ndk::BionicRuntime& rt, const char* so_path) {
  const char* classes = std::getenv("ACL_NDK_TEST_CLASSES");
  if (!classes) {
    std::fprintf(stderr, "--realvm needs ACL_NDK_TEST_CLASSES (dir with com/acl/ndktest/Main.class)\n");
    return false;
  }
  std::string cp = std::string("-Djava.class.path=") + classes;
  JavaVMOption opt{};
  opt.optionString = const_cast<char*>(cp.c_str());
  JavaVMInitArgs args{};
  args.version = JNI_VERSION_1_6;
  args.nOptions = 1;
  args.options = &opt;
  args.ignoreUnrecognized = JNI_FALSE;
  JavaVM* jvm = nullptr;
  JNIEnv* env = nullptr;
  if (JNI_CreateJavaVM(&jvm, reinterpret_cast<void**>(&env), &args) != JNI_OK) {
    std::fprintf(stderr, "JNI_CreateJavaVM failed\n");
    return false;
  }
  std::string err;
  if (!rt.load_library(so_path, jvm, env, &err)) {
    std::fprintf(stderr, "load_library: %s\n", err.c_str());
    return false;
  }
  jclass c = env->FindClass("com/acl/ndktest/Main");
  jmethodID mid = c ? env->GetStaticMethodID(c, "answer", "()I") : nullptr;
  if (!mid) { std::fprintf(stderr, "resolve Main.answer failed\n"); return false; }
  jint r = env->CallStaticIntMethod(c, mid);  // JVM dispatch -> down-call trampoline -> bionic native
  const bool ok = (r == 4250);
  std::printf("acl-ndk-probe: REAL JVM Main.answer() = %d %s\n", r, ok ? "(PASS)" : "(FAIL)");
  return ok;
}
#endif  // ACL_HAVE_JNI

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <abs-path-to.so> <symbol>|--boundary\n", argv[0]);
    return 2;
  }
  acl::ndk::BionicConfig cfg = acl::ndk::BionicConfig::from_env();
  std::string err;
  acl::ndk::BionicRuntime* rt = acl::ndk::BionicRuntime::instance(cfg, &err);
  if (!rt) {
    std::fprintf(stderr, "bootstrap failed: %s\n", err.c_str());
    return 1;
  }
  void* h = rt->load(argv[1], &err);
  if (!h) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }

  // Boundary mode: prove a bionic .so can call back into glibc and return with TLS intact.
  if (std::string(argv[2]) == "--boundary") {
    void* fn = rt->symbol(h, "acl_ndk_boundary");
    if (!fn) { std::fprintf(stderr, "symbol 'acl_ndk_boundary' not found\n"); return 1; }
    errno = 0;
    const int r = rt->run_boundary_selftest(fn, &glibc_cb, 5);  // expect 5*2 + 1000 = 1010
    std::printf("acl-ndk-probe: boundary selftest = %d %s\n", r, r == 1010 ? "(PASS)" : "(FAIL)");
    return r == 1010 ? 0 : 1;
  }

#if ACL_HAVE_JNI
  // JNI mode: drive the bionic .so's real JNI_OnLoad (GetEnv + FindClass + RegisterNatives) across
  // the %fs boundary, using a FAKE glibc "ART" JavaVM/JNIEnv. Then invoke the RegisterNatives'd
  // native. Proves the JNIEnv/JavaVM trampoline table (BionicRuntime::wrap_java_vm).
  if (std::string(argv[2]) == "--jni") {
    if (run_jni_selftest(*rt, argv[1])) return 0;
    return 1;
  }
  if (std::string(argv[2]) == "--realvm") {
    return run_realvm(*rt, argv[1]) ? 0 : 1;
  }
#endif
  void* fn = rt->symbol(h, argv[2]);
  if (!fn) {
    std::fprintf(stderr, "symbol '%s' not found\n", argv[2]);
    return 1;
  }
  int result;
  {
    acl::ndk::BionicRuntime::RunScope scope(*rt);
    result = reinterpret_cast<int (*)(void*, void*)>(fn)(nullptr, nullptr);
  }
  std::printf("acl-ndk-probe: %s() = %d %s\n", argv[2], result, result == 42 ? "(PASS)" : "");
  return result == 42 ? 0 : 1;
}

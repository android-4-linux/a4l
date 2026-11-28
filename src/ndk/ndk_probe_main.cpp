// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl. Mirrors the other src/**/ *_probe mains. Exercises
// the in-process bionic loader (src/ndk/bionic_runtime) against a bionic .so.
// No GPL-3.0 sources.
//
// Usage: acl-ndk-probe <target.so> <symbol>
//   env: ACL_BIONIC_LINKER, ACL_BIONIC_SYSROOT, ACL_BIONIC_STUB (see
//   docs/ndk-scope.md).
// Loads the bionic .so in-process on the glibc host, resolves <symbol>, and
// calls it as an int(JNIEnv*,jclass) (the tools/ndk-loader-spike repro's
// nativeAnswer ignores both args). Prints the returned int; exit 0 iff it
// returns 42 (the repro's value).

#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ndk/bionic_runtime.h"

// A glibc-side callback for the boundary self-test: writes glibc TLS (errno)
// and doubles x. If the up-call trampoline failed to restore glibc %fs, this
// errno write would corrupt bionic's TLS.
static int glibc_cb(int x) {
  errno = 99;
  return x * 2;
}

#if ACL_HAVE_JNI
// A minimal FAKE "ART" JavaVM/JNIEnv living on the glibc side. Each function
// writes glibc TLS (probe_tls) — if a trampoline failed to restore glibc %fs,
// that write would land in bionic's TLS and the values below would not survive.
// RegisterNatives captures the bound function pointer.
namespace {
const void* g_captured_native = nullptr;
// A second native, registered as "many" by a library that has one: twelve
// integer and nine floating-point arguments, so some of each are passed on the
// stack whatever the architecture.
const void* g_captured_many = nullptr;
int g_glibc_calls = 0;
__thread int probe_tls;  // glibc TLS, exercised during the up-calls

jint JNICALL fake_GetVersion(JNIEnv*) {
  probe_tls = 111;
  ++g_glibc_calls;
  return JNI_VERSION_1_6;
}
jclass JNICALL fake_FindClass(JNIEnv*, const char*) {
  probe_tls = 222;
  ++g_glibc_calls;
  return reinterpret_cast<jclass>(0x1234);
}
jint JNICALL fake_RegisterNatives(JNIEnv*,
                                  jclass,
                                  const JNINativeMethod* m,
                                  jint n) {
  probe_tls = 333;
  ++g_glibc_calls;
  if (n >= 1)
    g_captured_native = m[0].fnPtr;
  for (jint i = 1; i < n; ++i)
    if (m[i].name != nullptr && std::strcmp(m[i].name, "many") == 0)
      g_captured_many = m[i].fnPtr;
  return JNI_OK;
}
// The wrapped RegisterNatives records each binding (so name resolution won't
// overwrite it), which needs NewGlobalRef. No GC here, so handing back the same
// ref is a faithful enough fake. Deliberately NOT counted in g_glibc_calls:
// that counter means "up-calls the .so made", and this is acl's own bookkeeping
// inside the RegisterNatives thunk, not a call the .so asked for.
jobject JNICALL fake_NewGlobalRef(JNIEnv*, jobject o) {
  return o;
}
jmethodID JNICALL fake_GetStaticMethodID(JNIEnv*,
                                         jclass,
                                         const char*,
                                         const char*) {
  ++g_glibc_calls;
  return reinterpret_cast<jmethodID>(0x2);
}
jint JNICALL fake_CallStaticIntMethodV(JNIEnv*, jclass, jmethodID, va_list a) {
  probe_tls = 444;
  ++g_glibc_calls;
  int x = va_arg(a, int),
      y = va_arg(
          a, int);  // pulls the varargs forwarded from w_CallStaticIntMethod
  return x + y;
}
acl::ndk::JniEnvTable g_fake_env_tbl;
struct FakeEnv {
  const acl::ndk::JniEnvTable* functions;
} g_fake_env;

jint JNICALL fake_GetEnv(JavaVM*, void** out, jint) {
  ++g_glibc_calls;
  *out = &g_fake_env;
  return JNI_OK;
}
acl::ndk::JniVmTable g_fake_vm_tbl;
struct FakeVM {
  const acl::ndk::JniVmTable* functions;
} g_fake_vm;
// --glforward: a4l's libEGL.so and libGLESv2.so forward each entry point to
// host Mesa on a donor thread's thread pointer. Here the probe is Mesa: every
// slot of both bridges is gl_check, and the donor is a parked thread of the
// probe's. The target library calls one EGL and one GLES entry point with
// twelve integer and nine floating-point arguments, so some of each cross on
// the stack.
thread_local int t_gl_where = 0;  // its address tells the threads apart
int* g_gl_donor_where = nullptr;
int g_gl_calls = 0;
int g_gl_on_donor = 0;

long gl_check(long a1,
              long a2,
              long a3,
              long a4,
              long a5,
              long a6,
              long a7,
              long a8,
              long a9,
              long a10,
              long a11,
              long a12,
              double d1,
              double d2,
              double d3,
              double d4,
              double d5,
              double d6,
              double d7,
              double d8,
              double d9) {
  ++g_gl_calls;
  if (&t_gl_where == g_gl_donor_where)
    ++g_gl_on_donor;
  const double doubles = d1 * 1 + d2 * 2 + d3 * 3 + d4 * 4 + d5 * 5 + d6 * 6 +
                         d7 * 7 + d8 * 8 + d9 * 9;
  return a1 * 1 + a2 * 2 + a3 * 3 + a4 * 4 + a5 * 5 + a6 * 6 + a7 * 7 + a8 * 8 +
         a9 * 9 + a10 * 10 + a11 * 11 + a12 * 12 +
         static_cast<long>(doubles) * 1000;
}

struct GlBridge {
  std::uintptr_t donor;
  void* fns[512];
};

bool run_gl_forward_selftest(acl::ndk::BionicRuntime& rt, void* h) {
  auto* pool = static_cast<std::uintptr_t*>(rt.symbol(h, "acl_gl_donor_pool"));
  auto** egl = static_cast<void**>(rt.symbol(h, "acl_gl_egl_bridge"));
  auto** gles = static_cast<void**>(rt.symbol(h, "acl_gl_gles_bridge"));
  void* fn = rt.symbol(h, "acl_gl_forward_test");
  if (!pool || !egl || !gles || !fn) {
    std::fprintf(stderr,
                 "the target does not link a4l's libEGL.so and libGLESv2.so, "
                 "or has no acl_gl_forward_test\n");
    return false;
  }
  // The donor: a thread that reports its thread pointer and waits.
  std::mutex mu;
  std::condition_variable cv;
  std::uintptr_t donor_tp = 0;
  bool done = false;
  std::thread donor([&] {
    std::unique_lock<std::mutex> lk(mu);
    g_gl_donor_where = &t_gl_where;
    donor_tp = reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer());
    cv.notify_all();
    cv.wait(lk, [&] { return done; });
  });
  {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return donor_tp != 0; });
  }
  static GlBridge bridge;
  for (void*& slot : bridge.fns)
    slot = reinterpret_cast<void*>(&gl_check);
  for (int i = 0; i < 8; ++i)
    pool[i] = donor_tp;
  *egl = &bridge;
  *gles = &bridge;

  long r;
  {
    acl::ndk::BionicRuntime::RunScope scope(rt);
    r = reinterpret_cast<long (*)()>(fn)();
  }
  {
    std::lock_guard<std::mutex> lk(mu);
    done = true;
  }
  cv.notify_all();
  donor.join();

  // 1*1 + ... + 12*12 = 650; 0.5*1 + ... + 8.5*9 = 262.5, truncated, times
  // 1000. Twice.
  const long want = 2 * (650 + 262 * 1000);
  const bool ok = r == want && g_gl_calls == 2 && g_gl_on_donor == 2 &&
                  &t_gl_where != g_gl_donor_where;
  std::printf(
      "acl-ndk-probe: GL forwarders = %ld (want %ld), %d calls, %d on the "
      "donor %s\n",
      r, want, g_gl_calls, g_gl_on_donor, ok ? "(PASS)" : "(FAIL)");
  return ok;
}

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
  g_captured_many = nullptr;

  // The exact operation Runtime.nativeLoad will call: load + drive JNI_OnLoad
  // across the boundary.
  std::string err;
  if (!rt.load_library(so_path, real_vm, real_env, &err)) {
    std::fprintf(stderr, "load_library failed: %s\n", err.c_str());
    return false;
  }
  const jint ver =
      JNI_VERSION_1_6;  // load_library validated JNI_OnLoad's requested version

  // Invoke the registered native exactly as ART would: from GLIBC %fs, and what
  // RegisterNatives handed ART is the down-call TRAMPOLINE (not the raw bionic
  // native). The trampoline swaps to bionic %fs and substitutes the wrapped
  // env; the native's CallIntMethod up-call trampolines back to glibc.
  // Round-trip: 4200 + (20+30) = 4250. (No RunScope here — glibc %fs, like
  // ART.)
  jint native_ret = -1;
  if (g_captured_native) {
    native_ret = reinterpret_cast<jint (*)(JNIEnv*, jclass)>(
        const_cast<void*>(g_captured_native))(real_env, nullptr);
  }

  // 6 up-calls: GetEnv, GetVersion, FindClass, RegisterNatives (JNI_OnLoad) +
  // GetStaticMethodID, CallStaticIntMethod (native).
  // Every argument weighted by its place, so one out of order or read from
  // the wrong slot changes the sum: 1*1 + 2*2 + ... + 10*10 = 385, and
  // (0.5*1 + 1.5*2 + ... + 8.5*9) = 262.5, truncated and times 1000.
  bool many_ok = true;
  if (g_captured_many) {
    using Many =
        jlong (*)(JNIEnv*, jclass, jint, jint, jint, jint, jint, jint, jint,
                  jint, jint, jint, jdouble, jdouble, jdouble, jdouble, jdouble,
                  jdouble, jdouble, jdouble, jdouble);
    const jlong got =
        reinterpret_cast<Many>(const_cast<void*>(g_captured_many))(
            real_env, nullptr, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0.5, 1.5, 2.5,
            3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
    many_ok = got == 385 + 262 * 1000;
    if (!many_ok)
      std::fprintf(stderr, "many-argument native returned %lld\n",
                   static_cast<long long>(got));
  }
  const bool ok = ver == JNI_VERSION_1_6 && g_captured_native != nullptr &&
                  g_glibc_calls == 6 && probe_tls == 444 &&
                  native_ret == 4250 && many_ok;
  std::printf(
      "acl-ndk-probe: JNI_OnLoad ver=%#x up-calls=%d probe_tls=%d native=%d "
      "(varargs up-call) stack-args=%s -> %s\n",
      ver, g_glibc_calls, probe_tls, native_ret,
      g_captured_many ? (many_ok ? "ok" : "wrong") : "none",
      ok ? "(PASS)" : "(FAIL)");
  return ok;
}

#if defined(ACL_NDK_PROBE_REALVM)
// End-to-end against a REAL JVM (the host OpenJDK linked via jni_dep) — the
// strongest proof that the jvm_vm.cpp Runtime.nativeLoad hook will work: boot a
// GL-through-the-host mode: load the library the way an app's is loaded, which
// points the GL forwarders it links at the host's GL library, and call its
// acl_gl_host_call(). Which forwarder the call goes through is the library's
// choice: the one it links. 42 passes.
static bool run_gl_host_call(acl::ndk::BionicRuntime& rt, const char* so_path) {
  g_fake_env_tbl = {};
  g_fake_env_tbl.GetVersion = fake_GetVersion;
  g_fake_env.functions = &g_fake_env_tbl;
  g_fake_vm_tbl = {};
  g_fake_vm_tbl.GetEnv = fake_GetEnv;
  g_fake_vm.functions = &g_fake_vm_tbl;
  std::string err;
  void* h = rt.load_library(so_path, reinterpret_cast<JavaVM*>(&g_fake_vm),
                            reinterpret_cast<JNIEnv*>(&g_fake_env), &err);
  if (h == nullptr) {
    std::fprintf(stderr, "load_library failed: %s\n", err.c_str());
    return false;
  }
  void* fn = rt.symbol(h, "acl_gl_host_call");
  if (fn == nullptr) {
    std::fprintf(stderr, "symbol 'acl_gl_host_call' not found\n");
    return false;
  }
  int result;
  {
    const acl::ndk::BionicRuntime::RunScope scope(rt);
    result = reinterpret_cast<int (*)()>(fn)();
  }
  std::printf(
      "acl-ndk-probe: a GL call through the library's forwarder "
      "returned %d %s\n",
      result, result == 42 ? "(PASS)" : "(FAIL)");
  return result == 42;
}

// genuine JVM, load_library the bionic .so (drives JNI_OnLoad ->
// FindClass(Main) -> RegisterNatives(answer) with a down-call trampoline), then
// call Main.answer() from Java. The JVM's own dispatch invokes the trampoline
// -> bionic native -> GetStaticMethodID/CallStaticIntMethod(helper,20,30)
// up-calls -> back. Expect 4250.
static bool run_realvm(acl::ndk::BionicRuntime& rt, const char* so_path) {
  const char* classes = std::getenv("ACL_NDK_TEST_CLASSES");
  if (!classes) {
    std::fprintf(stderr,
                 "--realvm needs ACL_NDK_TEST_CLASSES (dir with "
                 "com/acl/ndktest/Main.class)\n");
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
  if (!mid) {
    std::fprintf(stderr, "resolve Main.answer failed\n");
    return false;
  }
  jint r = env->CallStaticIntMethod(
      c, mid);  // JVM dispatch -> down-call trampoline -> bionic native
  const bool ok = (r == 4250);
  std::printf("acl-ndk-probe: REAL JVM Main.answer() = %d %s\n", r,
              ok ? "(PASS)" : "(FAIL)");
  return ok;
}
#endif  // ACL_NDK_PROBE_REALVM
#endif  // ACL_HAVE_JNI

// Mappings currently owned by the process. An attached thread that never gave
// its TCB back leaves two behind, so this is how the churn loop below sees a
// leak.
static std::size_t count_mappings() {
  std::FILE* f = std::fopen("/proc/self/maps", "r");
  if (f == nullptr)
    return 0;
  std::size_t lines = 0;
  for (int c; (c = std::fgetc(f)) != EOF;)
    if (c == '\n')
      ++lines;
  std::fclose(f);
  return lines;
}

// Thread mode: attach a SECOND thread to bionic and run bionic code as that
// thread.
//
// This is the whole point of attach_current_thread: bionic created no TCB for
// this thread (glibc did), so acl builds one. What it proves, on the worker
// thread and against real bionic libc:
//   - the synthesized TCB survives real bionic code — nativeAnswer() calls
//   __android_log_print,
//     which goes through liblog into bionic libc and touches its TLS. A wrong
//     TCB (or a bad stack canary, which lives in a TCB slot) crashes here
//     rather than returning 42.
//   - pthread_self() returns THIS thread's pthread_internal_t (slot
//   TLS_SLOT_THREAD_ID), not the
//     bootstrapping thread's — the field that backs mutex ownership, so sharing
//     it would break mutual exclusion.
//   - bionic's gettid() agrees with the kernel's for this thread.
static bool run_thread_selftest(acl::ndk::BionicRuntime& rt, void* h) {
  auto* native_answer = rt.symbol(h, "Java_com_acl_ndktest_Main_nativeAnswer");
  auto* bionic_pthread_self =
      rt.symbol(h, "pthread_self");  // resolves through the .so to libc
  auto* bionic_gettid = rt.symbol(h, "gettid");
  if (!native_answer || !bionic_pthread_self || !bionic_gettid) {
    std::fprintf(stderr,
                 "thread selftest: missing symbol(s) in the target/libc\n");
    return false;
  }

  bool ok = false;
  std::thread worker([&] {
    std::string why;
    const std::uintptr_t fs = rt.attach_current_thread(&why);
    if (fs == 0) {
      std::fprintf(stderr, "attach failed: %s\n", why.c_str());
      return;
    }
    if (fs == rt.bootstrap_fs()) {
      std::fprintf(stderr,
                   "attach handed back the bootstrapping thread's TCB\n");
      return;
    }
    const auto kernel_tid = static_cast<long>(::syscall(SYS_gettid));
    int answer = -1;
    long tid = -1;
    void* self = nullptr;
    {
      acl::ndk::BionicRuntime::RunScope scope(rt);  // this thread's bionic %fs
      answer = reinterpret_cast<int (*)(void*, void*)>(native_answer)(nullptr,
                                                                      nullptr);
      self = reinterpret_cast<void* (*)()>(bionic_pthread_self)();
      tid = reinterpret_cast<long (*)()>(bionic_gettid)();
    }
    const bool self_ok =
        self != nullptr && self != rt.bootstrap_thread_struct();
    std::printf(
        "acl-ndk-probe: worker attached (tcb=%#lx) nativeAnswer=%d bionic "
        "gettid=%ld "
        "(kernel %ld) pthread_self=%p %s\n",
        static_cast<unsigned long>(fs), answer, tid, kernel_tid, self,
        (answer == 42 && tid == kernel_tid && self_ok) ? "(PASS)" : "(FAIL)");
    ok = answer == 42 && tid == kernel_tid && self_ok;
  });
  worker.join();
  if (!ok)
    return false;

  // Churn: attach and exit more threads than the fs-pair table has slots, and
  // more than once, so both halves of the release path are exercised — the
  // table slot must be reused (an append-only table aborts partway through) and
  // the thread's TLS block + thread struct must be unmapped (a leak shows up as
  // two mappings per thread, i.e. hundreds). This is the whole reason attach
  // registers a glibc key destructor: bionic never runs a thread-exit path for
  // a thread it did not create.
  const std::size_t maps_before = count_mappings();
  constexpr int kChurn = 300;  // > kMaxAttached (256) in bionic_runtime.cpp
  bool churn_ok = true;
  for (int i = 0; i < kChurn && churn_ok; ++i) {
    std::thread t([&] {
      std::string why;
      if (rt.attach_current_thread(&why) == 0) {
        churn_ok = false;
        return;
      }
      acl::ndk::BionicRuntime::RunScope scope(rt);
      if (reinterpret_cast<int (*)(void*, void*)>(native_answer)(nullptr,
                                                                 nullptr) != 42)
        churn_ok = false;
    });
    t.join();
  }
  const std::size_t maps_after = count_mappings();
  // Each leaked thread would add its TLS block + thread struct; glibc's own
  // stack caching moves this by a few either way, so allow slack while still
  // catching a real leak (~600).
  const bool no_leak = maps_after <= maps_before + 32;
  std::printf(
      "acl-ndk-probe: churned %d attach/exit cycles; mappings %zu -> %zu %s\n",
      kChurn, maps_before, maps_after,
      (churn_ok && no_leak) ? "(PASS)" : "(FAIL)");
  return churn_ok && no_leak;
}

// A thread made before the linker is bootstrapped, which is what ART's threads
// are in a4l: the linker comes up on the first System.loadLibrary. Released, it
// attaches and has the library make a system call with a path from bionic's
// allocator (acl_ndk_heap_path in tests/fixtures/ndk_answer.c). On arm64 that
// pointer carries a tag, and the kernel takes it only from a thread with the
// tagged address ABI on, which this one has only if the process asked for it
// before the thread was made.
class EarlyThread {
 public:
  EarlyThread() : th_([this] { run(); }) {}
  ~EarlyThread() { release(nullptr, nullptr); }
  EarlyThread(const EarlyThread&) = delete;
  EarlyThread& operator=(const EarlyThread&) = delete;

  // The library's errno, 0 when the call worked; -1 if it did not run.
  int release(acl::ndk::BionicRuntime* rt, void* fn) {
    {
      const std::lock_guard<std::mutex> lk(mu_);
      if (!released_) {
        rt_ = rt;
        fn_ = fn;
        released_ = true;
      }
    }
    cv_.notify_one();
    if (th_.joinable())
      th_.join();
    return result_;
  }
  unsigned top_byte() const { return top_byte_; }

 private:
  void run() {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return released_; });
    }
    if (rt_ == nullptr || fn_ == nullptr)
      return;
    std::string why;
    if (rt_->attach_current_thread(&why) == 0) {
      std::fprintf(stderr, "attach failed: %s\n", why.c_str());
      return;
    }
    const acl::ndk::BionicRuntime::RunScope scope(*rt_);
    result_ = reinterpret_cast<int (*)(unsigned*)>(fn_)(&top_byte_);
  }

  std::mutex mu_;
  std::condition_variable cv_;
  acl::ndk::BionicRuntime* rt_ = nullptr;
  void* fn_ = nullptr;
  bool released_ = false;
  int result_ = -1;
  unsigned top_byte_ = 0;
  std::thread th_;  // last: it runs as soon as it is made
};

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(
        stderr,
        "usage: %s <abs-path-to.so> "
        "<symbol>|--boundary|--jni|--realvm|--thread|--looper|--mediaformat|"
        "--load|--glforward|--glhost\n",
        argv[0]);
    return 2;
  }
  std::unique_ptr<EarlyThread> early;
  if (std::string(argv[2]) == "--thread")
    early = std::make_unique<EarlyThread>();
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

  // Looper mode: drive acl's libandroid.so (ALooper) from the bionic .so, the
  // way a real engine does — see Java_com_acl_ndktest_Main_looperTest in
  // tools/build-ndk-repro.sh for what 55 means.
  if (std::string(argv[2]) == "--looper") {
    void* fn = rt->symbol(h, "Java_com_acl_ndktest_Main_looperTest");
    if (!fn) {
      std::fprintf(stderr,
                   "looperTest not in the target (staged libandroid.so?)\n");
      return 1;
    }
    int r;
    {
      acl::ndk::BionicRuntime::RunScope scope(*rt);
      r = reinterpret_cast<int (*)(void*, void*)>(fn)(nullptr, nullptr);
    }
    std::printf(
        "acl-ndk-probe: ALooper round trip (prepare/addFd/wake/pollOnce) = %d "
        "%s\n",
        r, r == 55 ? "(PASS)" : "(FAIL)");
    return r == 55 ? 0 : 1;
  }

  // Media format mode: drive a4l's libmediandk.so from a bionic .so that
  // links it - see Java_com_acl_ndktest_Main_mediaFormatTest in
  // tools/build-ndk-repro.sh for the steps and what 77 means.
  if (std::string(argv[2]) == "--mediaformat") {
    void* fn = rt->symbol(h, "Java_com_acl_ndktest_Main_mediaFormatTest");
    if (!fn) {
      std::fprintf(
          stderr,
          "mediaFormatTest not in the target (staged libmediandk.so?)\n");
      return 1;
    }
    int r;
    {
      acl::ndk::BionicRuntime::RunScope scope(*rt);
      r = reinterpret_cast<int (*)(void*, void*)>(fn)(nullptr, nullptr);
    }
    std::printf("acl-ndk-probe: AMediaFormat and AMediaCodec = %d %s\n", r,
                r == 77 ? "(PASS)" : "(FAIL)");
    return r == 77 ? 0 : 1;
  }

  // Load mode: just load the .so in-process and report. For a REAL app's
  // library, where we do not know a safe symbol to call — the point is whether
  // the bionic linker can map it and satisfy its DT_NEEDED out of the staged
  // sysroot + ACL_BIONIC_APP_LIBS.
  if (std::string(argv[2]) == "--load") {
    std::printf("acl-ndk-probe: loaded %s (PASS)\n", argv[1]);
    return 0;
  }

  // GL forwarder mode: see run_gl_forward_selftest.
  if (std::string(argv[2]) == "--glforward") {
    return run_gl_forward_selftest(*rt, h) ? 0 : 1;
  }
  if (std::string(argv[2]) == "--glhost") {
    return run_gl_host_call(*rt, argv[1]) ? 0 : 1;
  }

  // Thread mode: attach a second thread and run bionic code as that thread.
  if (std::string(argv[2]) == "--thread") {
    // Not every library this mode is run on has the function.
    if (void* heap_path = rt->symbol(h, "acl_ndk_heap_path")) {
      const int e = early->release(rt, heap_path);
      std::printf(
          "acl-ndk-probe: thread older than the linker: access() of an "
          "allocated path (top byte %#x) -> errno %d %s\n",
          early->top_byte(), e, e == 0 ? "(PASS)" : "(FAIL)");
      if (e != 0)
        return 1;
    }
    return run_thread_selftest(*rt, h) ? 0 : 1;
  }

  // Boundary mode: prove a bionic .so can call back into glibc and return with
  // TLS intact.
  if (std::string(argv[2]) == "--boundary") {
    void* fn = rt->symbol(h, "acl_ndk_boundary");
    if (!fn) {
      std::fprintf(stderr, "symbol 'acl_ndk_boundary' not found\n");
      return 1;
    }
    errno = 0;
    const int r = rt->run_boundary_selftest(fn, &glibc_cb,
                                            5);  // expect 5*2 + 1000 = 1010
    std::printf("acl-ndk-probe: boundary selftest = %d %s\n", r,
                r == 1010 ? "(PASS)" : "(FAIL)");
    return r == 1010 ? 0 : 1;
  }

#if ACL_HAVE_JNI
  // JNI mode: drive the bionic .so's real JNI_OnLoad (GetEnv + FindClass +
  // RegisterNatives) across the %fs boundary, using a FAKE glibc "ART"
  // JavaVM/JNIEnv. Then invoke the RegisterNatives'd native. Proves the
  // JNIEnv/JavaVM trampoline table (BionicRuntime::wrap_java_vm).
  if (std::string(argv[2]) == "--jni") {
    if (run_jni_selftest(*rt, argv[1]))
      return 0;
    return 1;
  }
  if (std::string(argv[2]) == "--realvm") {
#if defined(ACL_NDK_PROBE_REALVM)
    return run_realvm(*rt, argv[1]) ? 0 : 1;
#else
    std::fprintf(stderr, "--realvm: this build has no libjvm to link\n");
    return 1;
#endif
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
  std::printf("acl-ndk-probe: %s() = %d %s\n", argv[2], result,
              result == 42 ? "(PASS)" : "");
  return result == 42 ? 0 : 1;
}

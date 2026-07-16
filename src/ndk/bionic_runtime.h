// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design; mechanism proven in tools/ndk-loader-spike/ (docs/ndk-scope.md
// Stage 0b). Bootstrap contract from AOSP bionic linker source (Apache-2.0). libhybris (Apache-2.0)
// referenced for the two-libc %fs technique, not vendored. No GPL-3.0 sources.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "acl_config.h"
#if ACL_HAVE_JNI
#include <jni.h>
#include <type_traits>
#endif

namespace acl::ndk {

// True if the ELF at `path` is bionic-linked (DT_NEEDED "libc.so", vs glibc's "libc.so.6"). Used to
// route app native libs (bionic → BionicRuntime) apart from acl's own glibc supplemental libs.
bool is_bionic_elf(const char* path);

#if ACL_HAVE_JNI
// The JNI function-table struct types, derived from the `functions` member so this is portable
// across jni.h variants (OpenJDK names them JNINativeInterface_/JNIInvokeInterface_; Android drops
// the trailing underscore). Same ABI layout either way.
using JniEnvTable = std::remove_const_t<std::remove_pointer_t<decltype(JNIEnv::functions)>>;
using JniVmTable = std::remove_const_t<std::remove_pointer_t<decltype(JavaVM::functions)>>;
#endif
}  // namespace acl::ndk

namespace acl::ndk {

// Where the bionic runtime lives on the host: the AOSP dynamic linker, a directory of bionic
// runtime libs (libc/libm/libdl/liblog/libc++/ld-android.so), and a minimal bionic PIE exe the
// linker "loads" during bootstrap (its entry never runs). See docs/ndk-scope.md.
struct BionicConfig {
  std::string linker;                        // absolute path to linker64
  std::string sysroot;                       // dir of bionic runtime libs (LD_LIBRARY_PATH root)
  std::string stub;                          // absolute path to a minimal bionic PIE exe
  std::vector<std::string> extra_lib_paths;  // app native-lib dir(s), appended to LD_LIBRARY_PATH

  // Populated from ACL_BIONIC_LINKER / ACL_BIONIC_SYSROOT / ACL_BIONIC_STUB.
  static BionicConfig from_env();
  // True iff linker/sysroot/stub are set, absolute, and exist. `why` gets the first problem.
  bool valid(std::string* why) const;
};

// The bionic linker bootstrapped in-process. Process-singleton: __linker_init can run only once,
// and a bad bootstrap input makes the linker abort the process, so inputs are validated first.
//
// Threading: bootstrap sets up the calling (main) thread's bionic TCB. Every call that runs bionic
// code (dlopen/dlsym/loaded app functions) must hold the thread pointer as bionic — use RunScope.
// Calling bionic code from other host threads is Rung 4 (attach-as-bionic); not yet supported.
class BionicRuntime {
 public:
  // Bootstrap (or return the already-bootstrapped singleton). Returns nullptr and sets `error` if
  // the config is invalid, the arch is unsupported, or the bootstrap fails. Idempotent.
  static BionicRuntime* instance(const BionicConfig& cfg, std::string* error);
  // The bootstrapped singleton, or nullptr if instance() has not succeeded.
  static BionicRuntime* get() noexcept;

  // dlopen a bionic .so (absolute path required; pre-validated). Returns a bionic handle or nullptr
  // + sets `error` (dlopen failure is graceful, not fatal). Runs with the thread pointer as bionic.
  void* load(const char* abs_path, std::string* error);
  // dlsym in a loaded handle. Runs with the thread pointer as bionic.
  void* symbol(void* handle, const char* name);

  // Bidirectional-boundary self-test (docs/ndk-scope.md Rung 4). Calls a loaded bionic function
  // `int f(int(*cb)(int), int)` under bionic %fs, passing an internal up-call trampoline that
  // restores glibc %fs around `cb` — proving a bionic .so can call back into glibc (JNI) and
  // return with both sides' TLS intact. Returns f's result, or -1 on unsupported arch.
  // (Main-thread only for now — uses the glibc %fs captured at bootstrap.)
  int run_boundary_selftest(void* bionic_fn, int (*cb)(int), int x);

#if ACL_HAVE_JNI
  // Wrap a real (glibc/ART) JavaVM so a bionic .so's JNI_OnLoad can drive it across the %fs
  // boundary: the returned JavaVM's GetEnv hands back a wrapped JNIEnv whose entry points are
  // %fs-restoring thunks over `real_env`'s. `real_env` is the ART JNIEnv for this thread. Returns
  // nullptr on unsupported arch. NOTE: only the slots a JNI_OnLoad/RegisterNatives path needs are
  // thunked so far (GetEnv/GetVersion/FindClass/RegisterNatives); the rest are copied through
  // unwrapped and will misbehave if called under bionic %fs — the full table is the next step.
  JavaVM* wrap_java_vm(JavaVM* real_vm, JNIEnv* real_env);
  // The wrapped JNIEnv produced by the last wrap_java_vm() — what a loaded bionic native should be
  // handed so its own JNI up-calls trampoline. Null before wrap_java_vm() succeeds.
  JNIEnv* wrapped_env() const { return wrap_env_; }

  // The operation the ART Runtime.nativeLoad hook calls: dlopen the app's bionic .so in-process,
  // and if it exports JNI_OnLoad, wrap ART's real JavaVM and drive JNI_OnLoad across the %fs
  // boundary (its RegisterNatives'd methods get down-call trampolines so ART can invoke them).
  // Returns the bionic handle, or nullptr + sets `error`. `real_vm`/`real_env` are ART's.
  void* load_library(const char* abs_path, JavaVM* real_vm, JNIEnv* real_env, std::string* error);

  // dlsym `sym` in `handle` and wrap it in a down-call trampoline ART can invoke directly. The raw
  // symbol must never be called by ART: it is bionic code expecting bionic %fs + the wrapped
  // JNIEnv, which is exactly what the trampoline substitutes. Null if the symbol is absent (the
  // normal "this native isn't exported by name" case) or on unsupported arch. load_library must
  // have run first (the trampoline needs the wrapped env). Used to bind natives by NAME —
  // JNI_OnLoad's RegisterNatives path gets its trampolines inside the wrapped RegisterNatives.
  void* downcall_symbol(void* handle, const char* sym);

  // True if a JNI_OnLoad already RegisterNatives'd (name, sig) on `klass`. Those bindings are the
  // app's explicit choice, so name resolution must not overwrite them (a .so may both export
  // Java_pkg_Class_m and register a *different* implementation for m).
  bool was_registered(JNIEnv* env, jclass klass, const char* name, const char* sig) const;
#endif

  // RAII: set the thread pointer to bionic for the scope's lifetime, restore glibc on exit. Wrap
  // any direct call into loaded app code in one of these. The swap itself is out-of-line in the
  // -fno-stack-protector TU (a mid-function %fs change would false-fail the caller's canary).
  class RunScope {
   public:
    explicit RunScope(BionicRuntime& rt);
    ~RunScope();
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;

   private:
    std::uintptr_t saved_;
  };

 private:
  BionicRuntime() = default;
  std::uintptr_t bionic_fs_ = 0;
  std::uintptr_t glibc_fs_ = 0;  // the bootstrapping (main) thread's glibc thread pointer
#if ACL_HAVE_JNI
  JNIEnv* wrap_env_ = nullptr;
#endif
  void* dlopen_fn_ = nullptr;
  void* dlsym_fn_ = nullptr;
  void* dlerror_fn_ = nullptr;
};

}  // namespace acl::ndk

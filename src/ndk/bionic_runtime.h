// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design; mechanism proven in tools/ndk-loader-spike/ (docs/ndk-scope.md
// Stage 0b). Bootstrap contract from AOSP bionic linker source (Apache-2.0). libhybris (Apache-2.0)
// referenced for the two-libc %fs technique, not vendored. No GPL-3.0 sources.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
  void* dlopen_fn_ = nullptr;
  void* dlsym_fn_ = nullptr;
  void* dlerror_fn_ = nullptr;
};

}  // namespace acl::ndk

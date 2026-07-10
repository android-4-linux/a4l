// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/runtime.md (pending), AOSP art host-embedding & JNI Invocation API
// docs, ADR-0001. No GPL-3.0 sources.
#ifndef ACLCXX_RUNTIME_VM_H
#define ACLCXX_RUNTIME_VM_H

// The runtime spine: a backend-agnostic seam for embedding a Java runtime,
// loading a boot/app classpath, and invoking a static main() — the Phase 1
// headless milestone (plan.md §4). Two real backends slot in behind this per
// ADR-0001: `Art` (host ART, the target — gated on the R-1 host build) and `Jvm`
// (the OpenJDK+Classpath-Exception fallback). `Null` executes no Java; it drives
// the plumbing (CLI, services) and tests before either lands.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aclcxx::runtime {

// ADR-0001: Art is the target; Jvm is the documented fallback; Null is the
// no-Java plumbing/test backend.
enum class Backend { Art, Jvm, Null };

const char* backend_name(Backend backend) noexcept;

// Parse a backend name ("art" | "jvm" | "null"), case-insensitive. Returns false
// on an unrecognized name.
bool parse_backend(std::string_view name, Backend& out) noexcept;

// Backend selected from the ACLCXX_VM environment variable, defaulting to Art.
Backend default_backend() noexcept;

// Where the runtime finds code and caches compiled output.
struct VmConfig {
  std::vector<std::string> boot_classpath;  // vendored framework dex/jars
  std::vector<std::string> classpath;       // app dex (extracted from the APK)
  std::string cache_dir;                     // DEX->OAT / dalvik-cache
  std::vector<std::string> options;          // raw VM options (-Xmx..., -verbose:gc)
};

enum class Status {
  Ok,
  Unavailable,    // backend not built/linked in this configuration (e.g. ART, R-1)
  InvalidConfig,
  NotStarted,     // operation requires a started VM
  NotFound,       // class or method not found
  NotApplicable,  // class exists but isn't the requested kind (e.g. not an Activity)
  RuntimeError,   // an exception escaped, or a VM-internal error
};

const char* status_name(Status status) noexcept;

// A lightweight error carrier. Truthy when it represents a failure.
struct Error {
  Status status = Status::Ok;
  std::string message;

  static Error ok() { return {}; }
  static Error of(Status s, std::string msg) { return {s, std::move(msg)}; }
  bool is_ok() const noexcept { return status == Status::Ok; }
  explicit operator bool() const noexcept { return status != Status::Ok; }
};

// The embedded runtime. One instance owns one VM. Not copyable.
class Vm {
 public:
  virtual ~Vm() = default;
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;

  // Bring the VM up per the config it was created with.
  virtual Error start() = 0;

  // Invoke `public static void main(String[])` on `main_class` (a binary or
  // dotted class name); `args` become the Java String[] contents. The Phase 1
  // headless-milestone entrypoint.
  virtual Error call_static_main(std::string_view main_class,
                                 const std::vector<std::string>& args) = 0;

  // Launch `activity_class` as an Android Activity: instantiate it and drive the
  // documented lifecycle (onCreate(null) -> onStart -> onResume). Requires
  // android.app.Activity on the boot classpath (the vendored/shimmed framework).
  // Returns Status::NotApplicable when the class exists but is not an Activity
  // subclass (the caller then falls back to call_static_main), NotFound when the
  // class is missing, RuntimeError on an escaped exception.
  virtual Error call_activity_lifecycle(std::string_view activity_class) = 0;

  // Tear the VM down. Safe to call once after start(); idempotent thereafter.
  virtual Error shutdown() = 0;

  Backend backend() const noexcept { return backend_; }

 protected:
  explicit Vm(Backend backend) : backend_(backend) {}

 private:
  Backend backend_;
};

// Create a VM for `backend`. Returns null and fills `out_error` (Unavailable)
// when the backend is not compiled/linked in this build — the current state for
// both `Art` (R-1) and `Jvm` (fallback not yet wired). `Null` always succeeds.
std::unique_ptr<Vm> create_vm(Backend backend, VmConfig config, Error& out_error);

}  // namespace aclcxx::runtime

#endif  // ACLCXX_RUNTIME_VM_H

// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/runtime.md (pending), JNI Invocation API docs, ADR-0001. No GPL-3.0 sources.

#include "runtime/vm.h"

#include <cstdlib>
#include <utility>

#include "aclcxx_config.h"
#include "log/log.h"

#if ACLCXX_HAVE_JNI
#include "runtime/jvm_vm.h"
#endif

namespace aclcxx::runtime {
namespace {

constexpr std::string_view kTag = "aclcxx.runtime";

// Case-insensitive ASCII equality for short backend names.
bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

std::string join(const std::vector<std::string>& parts, char sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out.push_back(sep);
    out += parts[i];
  }
  return out;
}

// No-Java backend. Records lifecycle and logs what a real backend would do.
// Executes no bytecode — it exists to bring up and test the plumbing (CLI,
// service hub, argument marshaling) before ART/JVM land.
class NullVm final : public Vm {
 public:
  explicit NullVm(VmConfig config) : Vm(Backend::Null), config_(std::move(config)) {}

  Error start() override {
    if (started_) return Error::ok();
    log::i(kTag, "VM(null) start: bootclasspath=[{}] classpath=[{}] cache='{}'",
           join(config_.boot_classpath, ':'), join(config_.classpath, ':'),
           config_.cache_dir);
    started_ = true;
    return Error::ok();
  }

  Error call_static_main(std::string_view main_class,
                         const std::vector<std::string>& args) override {
    if (!started_) {
      return Error::of(Status::NotStarted,
                       "call_static_main before start()");
    }
    log::i(kTag, "VM(null) would invoke {}.main([{}]) — no Java executed",
           main_class, join(args, ' '));
    return Error::ok();
  }

  Error shutdown() override {
    if (started_) {
      log::i(kTag, "VM(null) shutdown");
      started_ = false;
    }
    return Error::ok();
  }

 private:
  VmConfig config_;
  bool started_ = false;
};

}  // namespace

const char* backend_name(Backend backend) noexcept {
  switch (backend) {
    case Backend::Art:  return "art";
    case Backend::Jvm:  return "jvm";
    case Backend::Null: return "null";
  }
  return "?";
}

bool parse_backend(std::string_view name, Backend& out) noexcept {
  if (iequals(name, "art"))  { out = Backend::Art;  return true; }
  if (iequals(name, "jvm"))  { out = Backend::Jvm;  return true; }
  if (iequals(name, "null")) { out = Backend::Null; return true; }
  return false;
}

Backend default_backend() noexcept {
  if (const char* env = std::getenv("ACLCXX_VM")) {
    Backend b;
    if (parse_backend(env, b)) return b;
  }
  return Backend::Art;
}

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok:            return "ok";
    case Status::Unavailable:   return "unavailable";
    case Status::InvalidConfig: return "invalid-config";
    case Status::NotStarted:    return "not-started";
    case Status::NotFound:      return "not-found";
    case Status::RuntimeError:  return "runtime-error";
  }
  return "?";
}

std::unique_ptr<Vm> create_vm(Backend backend, VmConfig config, Error& out_error) {
  switch (backend) {
    case Backend::Null:
      out_error = Error::ok();
      return std::make_unique<NullVm>(std::move(config));

    case Backend::Art:
      // R-1 / ADR-0001: the host ART build is the hardest infrastructure item and
      // is not linked in this build. Vendor AOSP `art` (host) to enable it, or
      // select the JVM fallback.
      out_error = Error::of(
          Status::Unavailable,
          "host ART backend not available (R-1); vendor AOSP art (host build) "
          "or use --vm=jvm. See docs/adr/ADR-0001-dex-execution-host-art.md");
      return nullptr;

    case Backend::Jvm:
#if ACLCXX_HAVE_JNI
      // ADR-0001 fallback: OpenJDK via the JNI Invocation API. Runs stock JVM
      // bytecode; DEX support (d8-reversal) is a later step.
      out_error = Error::ok();
      return make_jvm_vm(std::move(config));
#else
      out_error = Error::of(
          Status::Unavailable,
          "JVM backend not built (no JDK found at configure time); rebuild with "
          "a JDK present, or use --vm=null. See docs/adr/ADR-0001-dex-execution-host-art.md");
      return nullptr;
#endif
  }
  out_error = Error::of(Status::InvalidConfig, "unknown backend");
  return nullptr;
}

}  // namespace aclcxx::runtime

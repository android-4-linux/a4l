// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.
//
// Isolated JVM integration test. HotSpot permits exactly one VM per process and
// no re-creation after DestroyJavaVM, so this lives in its own test binary and
// starts a single VM. Skipped unless the build compiled the Java fixtures and
// passed their directory via ACLCXX_JVM_CLASSES.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/vm.h"

using namespace aclcxx::runtime;

namespace {

std::string classes_dir() {
  const char* p = std::getenv("ACLCXX_JVM_CLASSES");
  return p ? p : std::string{};
}

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("JVM backend executes real Java through the seam", "[runtime][jvm]") {
  const std::string dir = classes_dir();
  if (dir.empty()) SKIP("ACLCXX_JVM_CLASSES not set (fixtures not built)");

  VmConfig cfg;
  cfg.classpath = {dir};

  Error err;
  auto vm = create_vm(Backend::Jvm, cfg, err);
  REQUIRE(vm != nullptr);
  REQUIRE(err.is_ok());
  REQUIRE(vm->backend() == Backend::Jvm);

  REQUIRE(vm->start().is_ok());

  // 1. main() actually runs: it writes a marker file we can observe.
  const std::filesystem::path marker =
      std::filesystem::temp_directory_path() / "aclcxx_jvm_marker.txt";
  std::error_code ec;
  std::filesystem::remove(marker, ec);

  Error r = vm->call_static_main("Main", {marker.string()});
  CHECK(r.is_ok());
  CHECK(std::filesystem::exists(marker));
  CHECK(read_file(marker) == "aclcxx-jvm-ok");

  // 2. Dotted class names are accepted (normalized to JNI form).
  CHECK(vm->call_static_main("Main", {}).is_ok());

  // 3. A missing class is reported, not fatal.
  Error missing = vm->call_static_main("does.not.Exist", {});
  CHECK(missing.status == Status::NotFound);

  // 4. An uncaught exception in main() maps to RuntimeError.
  Error boom = vm->call_static_main("Boom", {});
  CHECK(boom.status == Status::RuntimeError);

  CHECK(vm->shutdown().is_ok());
  std::filesystem::remove(marker, ec);
}

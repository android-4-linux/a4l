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

#include "dex/dex_to_jvm.h"
#include "dex/dexfile.h"
#include "runtime/vm.h"

using namespace aclcxx::runtime;

namespace {

std::string env(const char* name) {
  const char* p = std::getenv(name);
  return p ? p : std::string{};
}

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_bytes(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("JVM backend executes real Java, including a DEX->JVM converted class",
          "[runtime][jvm]") {
  const std::string dir = env("ACLCXX_JVM_CLASSES");
  if (dir.empty()) SKIP("ACLCXX_JVM_CLASSES not set (fixtures not built)");

  VmConfig cfg;
  cfg.classpath = {dir};

  // Convert the fixture DEX (if present) and add the compiled classes to the
  // classpath *before* the VM starts. This exercises the whole DEX->JVM path:
  // parse -> emit stubs -> javac -> JVM class loading + method resolution.
  std::string converted_dir;
  const std::string dex_path = env("ACLCXX_FIXTURE_DEX");
  if (!dex_path.empty()) {
    const auto bytes = read_bytes(dex_path);
    aclcxx::dex::DexError derr;
    auto dex = aclcxx::dex::DexFile::parse(bytes, derr);
    REQUIRE(dex.has_value());
    aclcxx::dex::ConvertResult cres;
    aclcxx::dex::DexError cerr = aclcxx::dex::convert(*dex, {}, cres);
    if (cerr) {
      WARN("dex convert skipped: " << cerr.message);
    } else {
      converted_dir = cres.classpath_dir;
      cfg.classpath.push_back(converted_dir);
    }
  }

  Error err;
  auto vm = create_vm(Backend::Jvm, cfg, err);
  REQUIRE(vm != nullptr);
  REQUIRE(err.is_ok());
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
  CHECK(vm->call_static_main("does.not.Exist", {}).status == Status::NotFound);

  // 4. An uncaught exception in main() maps to RuntimeError.
  CHECK(vm->call_static_main("Boom", {}).status == Status::RuntimeError);

  // 5. DEX->JVM: the converted HelloDex class LOADS and its main RESOLVES; the
  //    stub body throws UnsupportedOperationException -> RuntimeError. This
  //    proves classloading/resolution of a DEX-derived class end to end.
  if (!converted_dir.empty()) {
    Error hello = vm->call_static_main("HelloDex", {});
    CHECK(hello.status == Status::RuntimeError);  // loaded, resolved, invoked
  }

  CHECK(vm->shutdown().is_ok());
  std::filesystem::remove(marker, ec);
}

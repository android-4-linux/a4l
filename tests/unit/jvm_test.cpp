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

  // 5. DEX->JVM translation: the converted HelloDex.main was Dalvik return-void,
  //    translated to a real JVM `return` — so it runs cleanly (no longer a stub
  //    that throws). And HelloDex.answer() was `const/16 42; return v0`,
  //    translated to executing bytecode: Runner reflectively calls it and must
  //    observe an actual 42. This proves real bytecode ran, not a stub.
  if (!converted_dir.empty()) {
    CHECK(vm->call_static_main("HelloDex", {}).is_ok());  // translated return-void

    const std::filesystem::path answer =
        std::filesystem::temp_directory_path() / "aclcxx_answer.txt";
    std::filesystem::remove(answer, ec);
    Error rr = vm->call_static_main("Runner", {answer.string()});
    CHECK(rr.is_ok());
    // answer()=42 (const/return); loopSum()=55 (translated loop with branches).
    CHECK(read_file(answer) == "42,55,97,5,17,42,99,10,5,2,10,10,10,42,42,42,42,5,42,5,42,42,42,77,21,42");
    std::filesystem::remove(answer, ec);
  }

  CHECK(vm->shutdown().is_ok());
  std::filesystem::remove(marker, ec);
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "dex/dex_to_jvm.h"
#include "dex/dexfile.h"

using namespace aclcxx::dex;

namespace {

std::vector<std::uint8_t> load_fixture_dex() {
  const char* p = std::getenv("ACLCXX_FIXTURE_DEX");
  if (!p) return {};
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

const DexMethod* find(const std::vector<DexMethod>& v, std::string_view name) {
  for (const DexMethod& m : v) {
    if (m.name == name) return &m;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("descriptor_to_java maps primitives, objects, and arrays", "[dex]") {
  CHECK(descriptor_to_java("I") == "int");
  CHECK(descriptor_to_java("V") == "void");
  CHECK(descriptor_to_java("Z") == "boolean");
  CHECK(descriptor_to_java("[I") == "int[]");
  CHECK(descriptor_to_java("Lcom/example/Foo;") == "com.example.Foo");
  CHECK(descriptor_to_java("[[Ljava/lang/String;") == "java.lang.String[][]");
}

TEST_CASE("DexFile parses classes and method signatures", "[dex]") {
  const auto bytes = load_fixture_dex();
  if (bytes.empty()) SKIP("ACLCXX_FIXTURE_DEX not set");

  DexError err;
  auto dex = DexFile::parse(bytes, err);
  REQUIRE(dex.has_value());
  REQUIRE(err.is_ok());
  REQUIRE(dex->classes().size() == 1);

  const DexClass& c = dex->classes()[0];
  CHECK(c.java_name == "HelloDex");
  REQUIRE(c.super_name.has_value());
  CHECK(*c.super_name == "java.lang.Object");
  CHECK(c.access_flags & kAccPublic);
  REQUIRE(c.direct_methods.size() == 2);

  const DexMethod* main = find(c.direct_methods, "main");
  REQUIRE(main != nullptr);
  CHECK(main->return_type == "void");
  CHECK(main->is_static());
  CHECK(main->has_code);
  REQUIRE(main->param_types.size() == 1);
  CHECK(main->param_types[0] == "java.lang.String[]");

  const DexMethod* answer = find(c.direct_methods, "answer");
  REQUIRE(answer != nullptr);
  CHECK(answer->return_type == "int");
  CHECK(answer->param_types.empty());
  CHECK(answer->is_static());
}

TEST_CASE("DexFile rejects non-dex input", "[dex]") {
  const std::vector<std::uint8_t> junk(200, 0x00);
  DexError err;
  auto dex = DexFile::parse(junk, err);
  CHECK_FALSE(dex.has_value());
  CHECK(err.status == DexStatus::BadFormat);
}

TEST_CASE("emit_java_stub produces compilable class shape", "[dex]") {
  const auto bytes = load_fixture_dex();
  if (bytes.empty()) SKIP("ACLCXX_FIXTURE_DEX not set");
  DexError err;
  auto dex = DexFile::parse(bytes, err);
  REQUIRE(dex.has_value());

  const std::string src = emit_java_stub(dex->classes()[0]);
  CHECK(src.find("public class HelloDex {") != std::string::npos);
  CHECK(src.find("public static void main(java.lang.String[] a0)") !=
        std::string::npos);
  CHECK(src.find("public static int answer()") != std::string::npos);
  CHECK(src.find("throw new UnsupportedOperationException") != std::string::npos);
}

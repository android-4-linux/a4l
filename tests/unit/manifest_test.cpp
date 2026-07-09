// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "pkg/apk.h"
#include "pkg/manifest.h"

using namespace aclcxx::pkg;

namespace {

std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : std::string{};
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

// Assert the fields that both fixtures (UTF-8 and UTF-16) encode identically.
void check_known_manifest(const Manifest& m) {
  CHECK(m.package == "com.example.app");
  REQUIRE(m.version_code.has_value());
  CHECK(*m.version_code == 42);
  CHECK(m.version_name == "1.2.3");
  REQUIRE(m.min_sdk.has_value());
  CHECK(*m.min_sdk == 24);
  REQUIRE(m.target_sdk.has_value());
  CHECK(*m.target_sdk == 33);
  REQUIRE(m.compile_sdk.has_value());
  CHECK(*m.compile_sdk == 33);

  REQUIRE(m.activities.size() == 2);
  CHECK(m.activities[0].name == "com.example.app.MainActivity");  // ".MainActivity"
  CHECK(m.activities[0].launcher);
  CHECK(m.activities[1].name == "com.example.app.OtherActivity");
  CHECK_FALSE(m.activities[1].launcher);

  const Activity* launch = m.launcher();
  REQUIRE(launch != nullptr);
  CHECK(launch->name == "com.example.app.MainActivity");
}

}  // namespace

TEST_CASE("resolve_class_name applies Android shorthand rules", "[pkg][manifest]") {
  CHECK(resolve_class_name("com.x", ".Main") == "com.x.Main");
  CHECK(resolve_class_name("com.x", "Main") == "com.x.Main");
  CHECK(resolve_class_name("com.x", "a.b.Main") == "a.b.Main");
  CHECK(resolve_class_name("com.x", "") == "");
}

TEST_CASE("parse a UTF-8 AXML manifest read from the APK", "[pkg][manifest]") {
  const std::string apk = env("ACLCXX_FIXTURE_APK");
  if (apk.empty()) SKIP("ACLCXX_FIXTURE_APK not set");

  ZipError zerr;
  auto a = Apk::open(apk, zerr);
  REQUIRE(a.has_value());
  auto bytes = a->read("AndroidManifest.xml", zerr);
  REQUIRE(bytes.has_value());

  Manifest m;
  AxmlError err = parse_manifest(*bytes, m);
  REQUIRE(err.is_ok());
  check_known_manifest(m);
}

TEST_CASE("parse a UTF-16 AXML manifest (exercises UTF-16 string pool)",
          "[pkg][manifest]") {
  const std::string path = env("ACLCXX_AXML_UTF16");
  if (path.empty()) SKIP("ACLCXX_AXML_UTF16 not set");

  const std::vector<std::uint8_t> bytes = read_file(path);
  REQUIRE_FALSE(bytes.empty());

  Manifest m;
  AxmlError err = parse_manifest(bytes, m);
  REQUIRE(err.is_ok());
  check_known_manifest(m);
}

TEST_CASE("parse_manifest rejects non-AXML input", "[pkg][manifest]") {
  const std::vector<std::uint8_t> junk = {'n', 'o', 't', ' ', 'a', 'x', 'm', 'l'};
  Manifest m;
  AxmlError err = parse_manifest(junk, m);
  CHECK_FALSE(err.is_ok());
  CHECK(err.status == AxmlStatus::BadFormat);
}

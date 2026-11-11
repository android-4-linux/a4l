// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "pkg/apk.h"

using namespace acl::pkg;

namespace {

// Path to the build-time-generated fixture APK (see make_fixture_apk.py),
// passed via the test environment by meson.
std::string fixture_path() {
  const char* p = std::getenv("ACL_FIXTURE_APK");
  return p ? p : std::string{};
}

std::string as_text(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TEST_CASE("Apk opens a zip and indexes its central directory", "[pkg]") {
  const std::string path = fixture_path();
  if (path.empty())
    SKIP("ACL_FIXTURE_APK not set");

  ZipError err;
  auto apk = Apk::open(path, err);
  REQUIRE(apk.has_value());
  CHECK(err.is_ok());
  CHECK(apk->entries().size() == 9);
  CHECK(apk->find("classes.dex") != nullptr);
  CHECK(apk->find("does/not/exist") == nullptr);
}

TEST_CASE("Apk inflates DEFLATE and copies STORED entries", "[pkg]") {
  const std::string path = fixture_path();
  if (path.empty())
    SKIP("ACL_FIXTURE_APK not set");

  ZipError err;
  auto apk = Apk::open(path, err);
  REQUIRE(apk.has_value());

  // DEFLATE entry.
  auto hello = apk->read("assets/hello.txt", err);
  REQUIRE(hello.has_value());
  CHECK(as_text(*hello) == "hello acl\n");

  // STORED entry.
  auto mf = apk->read("META-INF/MANIFEST.MF", err);
  REQUIRE(mf.has_value());
  CHECK(as_text(*mf) == "Manifest-Version: 1.0\n");

  // A larger DEFLATE entry round-trips to its exact uncompressed size.
  const Apk::Entry* dex2 = apk->find("classes2.dex");
  REQUIRE(dex2 != nullptr);
  std::vector<std::uint8_t> out;
  REQUIRE(apk->read(*dex2, out, err));
  CHECK(out.size() == dex2->uncomp_size);
}

TEST_CASE("Apk enumerates distinct native ABIs, sorted", "[pkg]") {
  const std::string path = fixture_path();
  if (path.empty())
    SKIP("ACL_FIXTURE_APK not set");

  ZipError err;
  auto apk = Apk::open(path, err);
  REQUIRE(apk.has_value());

  const std::vector<std::string> abis = apk->native_abis();
  REQUIRE(abis.size() == 3);
  CHECK(abis[0] == "arm64-v8a");
  CHECK(abis[1] == "armeabi-v7a");
  CHECK(abis[2] == "x86_64");
}

TEST_CASE("Apk reports errors for missing files and entries", "[pkg]") {
  ZipError err;
  auto missing = Apk::open("/no/such/file.apk", err);
  CHECK_FALSE(missing.has_value());
  CHECK(err.status == ZipStatus::Io);

  const std::string path = fixture_path();
  if (path.empty())
    SKIP("ACL_FIXTURE_APK not set");
  auto apk = Apk::open(path, err);
  REQUIRE(apk.has_value());

  ZipError read_err;
  auto none = apk->read("nope.txt", read_err);
  CHECK_FALSE(none.has_value());
  CHECK(read_err.status == ZipStatus::NotFound);
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include "runtime/vm.h"

using namespace aclcxx::runtime;

TEST_CASE("backend name parsing round-trips and is case-insensitive", "[runtime]") {
  Backend b;
  REQUIRE(parse_backend("art", b));
  CHECK(b == Backend::Art);
  REQUIRE(parse_backend("JVM", b));
  CHECK(b == Backend::Jvm);
  REQUIRE(parse_backend("Null", b));
  CHECK(b == Backend::Null);
  CHECK_FALSE(parse_backend("dalvik", b));

  CHECK(std::string_view(backend_name(Backend::Art)) == "art");
  CHECK(std::string_view(backend_name(Backend::Jvm)) == "jvm");
  CHECK(std::string_view(backend_name(Backend::Null)) == "null");
}

TEST_CASE("ART and JVM backends report Unavailable, not a null crash", "[runtime]") {
  Error err;
  auto art = create_vm(Backend::Art, {}, err);
  CHECK(art == nullptr);
  CHECK(err.status == Status::Unavailable);
  CHECK_FALSE(err.message.empty());

  err = Error::ok();
  auto jvm = create_vm(Backend::Jvm, {}, err);
  CHECK(jvm == nullptr);
  CHECK(err.status == Status::Unavailable);
}

TEST_CASE("Null backend drives the start/invoke/shutdown lifecycle", "[runtime]") {
  VmConfig cfg;
  cfg.boot_classpath = {"framework.jar"};
  cfg.classpath = {"/tmp/app/classes.dex"};
  cfg.cache_dir = "/tmp/app/oat";

  Error err;
  auto vm = create_vm(Backend::Null, cfg, err);
  REQUIRE(vm != nullptr);
  CHECK(err.is_ok());
  CHECK(vm->backend() == Backend::Null);

  CHECK(vm->start().is_ok());
  CHECK(vm->call_static_main("com.example.Main", {"--flag", "value"}).is_ok());
  CHECK(vm->shutdown().is_ok());
}

TEST_CASE("Null backend rejects invoke before start", "[runtime]") {
  Error err;
  auto vm = create_vm(Backend::Null, {}, err);
  REQUIRE(vm != nullptr);

  Error r = vm->call_static_main("X", {});
  CHECK(r.status == Status::NotStarted);
  CHECK(static_cast<bool>(r));  // truthy: it is an error
}

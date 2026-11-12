// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "acl_config.h"
#include "runtime/vm.h"

using namespace acl::runtime;

TEST_CASE("backend name parsing round-trips and is case-insensitive",
          "[runtime]") {
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

TEST_CASE("ART backend availability tracks ACL_ART_HOME", "[runtime]") {
  // Snapshot + restore the env so the test is order-independent (other suites
  // may rely on ACL_ART_HOME being set).
  const char* saved = std::getenv("ACL_ART_HOME");
  const std::string saved_str = saved != nullptr ? saved : std::string();

  // No host ART build (ACL_ART_HOME unset): the Art backend reports Unavailable
  // gracefully — no null-deref crash.
  ::unsetenv("ACL_ART_HOME");
  {
    Error err;
    auto art = create_vm(Backend::Art, {}, err);
    CHECK(art == nullptr);
    CHECK(err.status == Status::Unavailable);
    CHECK_FALSE(err.message.empty());
  }

#if ACL_HAVE_JNI
  // With ACL_ART_HOME set (M2: host-ART embedding compiled in), create_vm
  // constructs the backend. Starting it would dlopen libart — out of scope for
  // a unit test — so a bogus path still constructs; only start() would fail.
  ::setenv("ACL_ART_HOME", "/nonexistent/acl-art-home", 1);
  {
    Error err;
    auto art = create_vm(Backend::Art, {}, err);
    CHECK(art != nullptr);
    CHECK(err.status == Status::Ok);
  }
#endif

  if (saved != nullptr) {
    ::setenv("ACL_ART_HOME", saved_str.c_str(), 1);
  } else {
    ::unsetenv("ACL_ART_HOME");
  }
}

TEST_CASE("JVM backend availability tracks the build", "[runtime]") {
  Error err;
  auto jvm = create_vm(Backend::Jvm, {}, err);
#if ACL_HAVE_JNI
  // Constructed but NOT started — HotSpot allows one VM per process, so the
  // shared unit binary never starts one (see the isolated jvm-invoke test).
  REQUIRE(jvm != nullptr);
  CHECK(jvm->backend() == Backend::Jvm);
  CHECK(err.is_ok());
#else
  CHECK(jvm == nullptr);
  CHECK(err.status == Status::Unavailable);
#endif
}

TEST_CASE("Null backend drives the start/invoke/shutdown lifecycle",
          "[runtime]") {
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

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l tests. No GPL-3.0 sources.
//
// svc/permission: reading the store's answer, the ACL_ALLOW_DEVICES list, and
// how long an answer is kept.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "svc/permission/permission_store.h"

namespace {

using acl::permission::Access;
using acl::permission::decode;
using acl::permission::DeviceAccess;
using acl::permission::listed;
using std::chrono::milliseconds;

}  // namespace

TEST_CASE("permission: the store's answer", "[permission]") {
  CHECK(decode({}) == Access::kUnset);
  CHECK(decode({"yes"}) == Access::kYes);
  CHECK(decode({"no"}) == Access::kNo);
  CHECK(decode({"ask"}) == Access::kAsk);
  CHECK(decode({"YES"}) == Access::kNo);
  CHECK(decode({""}) == Access::kNo);
  CHECK(decode({"yes", "no"}) == Access::kNo);
}

TEST_CASE("permission: the allow list", "[permission]") {
  CHECK(listed("microphone", "microphone"));
  CHECK(listed("camera,microphone", "microphone"));
  CHECK(listed(" camera , microphone ", "microphone"));
  CHECK_FALSE(listed("", "microphone"));
  CHECK_FALSE(listed("camera", "microphone"));
  CHECK_FALSE(listed("microphones", "microphone"));
  CHECK_FALSE(listed("micro,phone", "microphone"));
  CHECK_FALSE(listed(",,", "microphone"));
  CHECK_FALSE(listed("microphone", ""));
}

TEST_CASE("permission: only yes allows", "[permission]") {
  for (const Access a : {Access::kUnset, Access::kNo, Access::kAsk,
                         Access::kUnavailable, Access::kYes}) {
    DeviceAccess access(
        "", [a](const std::string&, const std::string&) { return a; });
    CHECK(access.allowed("microphone", "com.example") == (a == Access::kYes));
  }
}

TEST_CASE("permission: the allow list needs no store", "[permission]") {
  int calls = 0;
  DeviceAccess access("microphone",
                      [&calls](const std::string&, const std::string&) {
                        ++calls;
                        return Access::kNo;
                      });
  CHECK(access.allowed("microphone", "com.example"));
  CHECK_FALSE(access.allowed("camera", "com.example"));
  CHECK_FALSE(access.allowed("microphone", ""));
  CHECK_FALSE(access.allowed("", "com.example"));
  CHECK(calls == 1);
}

TEST_CASE("permission: an answer is kept for a second", "[permission]") {
  DeviceAccess::Clock::time_point now{};
  int calls = 0;
  Access answer = Access::kNo;
  DeviceAccess access(
      "",
      [&](const std::string&, const std::string&) {
        ++calls;
        return answer;
      },
      [&now] { return now; });

  CHECK_FALSE(access.allowed("microphone", "com.example"));
  answer = Access::kYes;
  now += milliseconds(999);
  CHECK_FALSE(access.allowed("microphone", "com.example"));
  CHECK(calls == 1);
  now += milliseconds(1);
  CHECK(access.allowed("microphone", "com.example"));
  CHECK(calls == 2);

  // Another app and another device are their own questions.
  answer = Access::kNo;
  CHECK_FALSE(access.allowed("microphone", "com.other"));
  CHECK_FALSE(access.allowed("camera", "com.example"));
  CHECK(access.allowed("microphone", "com.example"));
  CHECK(calls == 4);
}

TEST_CASE("permission: a missing store is asked again after thirty seconds",
          "[permission]") {
  DeviceAccess::Clock::time_point now{};
  int calls = 0;
  Access answer = Access::kUnavailable;
  DeviceAccess access(
      "",
      [&](const std::string&, const std::string&) {
        ++calls;
        return answer;
      },
      [&now] { return now; });

  CHECK_FALSE(access.allowed("microphone", "com.example"));
  answer = Access::kYes;
  now += std::chrono::seconds(29);
  CHECK_FALSE(access.allowed("microphone", "com.example"));
  CHECK(calls == 1);
  now += std::chrono::seconds(1);
  CHECK(access.allowed("microphone", "com.example"));
  CHECK(calls == 2);
}

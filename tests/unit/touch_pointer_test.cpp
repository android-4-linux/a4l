// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test code. No GPL-3.0 sources.
#include <catch2/catch_test_macros.hpp>

#include "wm/touch_pointer.h"

using acl::wm::PointerInput;
using acl::wm::TouchPointer;

TEST_CASE("the first finger down is the pointer until it lifts",
          "[wm][touch]") {
  TouchPointer t;
  CHECK_FALSE(t.active());

  const auto down = t.down(7, 10.0, 20.0, false);
  REQUIRE(down);
  CHECK(down->action == PointerInput::Action::Down);
  CHECK(down->x == 10.0F);
  CHECK(down->y == 20.0F);
  CHECK(t.active());

  // A second finger is not a second pointer, and does not move the first.
  CHECK_FALSE(t.down(8, 99.0, 99.0, false));
  CHECK_FALSE(t.motion(8, 98.0, 98.0));
  CHECK_FALSE(t.up(8));
  CHECK(t.active());

  const auto move = t.motion(7, 11.0, 22.0);
  REQUIRE(move);
  CHECK(move->action == PointerInput::Action::Move);
  CHECK(move->x == 11.0F);
  CHECK(move->y == 22.0F);

  // up has no position of its own: it is where the finger last was.
  const auto up = t.up(7);
  REQUIRE(up);
  CHECK(up->action == PointerInput::Action::Up);
  CHECK(up->x == 11.0F);
  CHECK(up->y == 22.0F);
  CHECK_FALSE(t.active());

  // Lifted: its motion and a second up are nothing, and the next finger is
  // the pointer.
  CHECK_FALSE(t.motion(7, 1.0, 1.0));
  CHECK_FALSE(t.up(7));
  CHECK(t.down(8, 5.0, 6.0, false));
}

TEST_CASE("a finger does not take the pointer from a held mouse button",
          "[wm][touch]") {
  TouchPointer t;
  CHECK_FALSE(t.down(1, 1.0, 1.0, true));
  CHECK_FALSE(t.active());
  CHECK_FALSE(t.up(1));
  CHECK_FALSE(t.down(-1, 1.0, 1.0, false));  // not a contact id
}

TEST_CASE("a canceled touch lifts outside the surface", "[wm][touch]") {
  TouchPointer t;
  CHECK_FALSE(t.cancel());  // nothing down
  REQUIRE(t.down(3, 40.0, 50.0, false));
  const auto up = t.cancel();
  REQUIRE(up);
  CHECK(up->action == PointerInput::Action::Up);
  CHECK(up->x < 0.0F);
  CHECK(up->y < 0.0F);
  CHECK_FALSE(t.active());
  CHECK_FALSE(t.cancel());
}

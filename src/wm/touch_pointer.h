// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l code; the wl_touch events as the Wayland protocol
// documents them, and android.view.MotionEvent's documented single-pointer
// actions. No GPL-3.0 sources.
#ifndef ACL_WM_TOUCH_POINTER_H
#define ACL_WM_TOUCH_POINTER_H

#include <cstdint>
#include <optional>

#include "wm/present.h"

namespace acl::wm {

// A touchscreen's contacts as the one pointer a PointerInput describes: the
// first finger down is the pointer until it lifts, and other fingers are
// ignored while it is down. Pinch and other gestures of two fingers are not
// carried.
class TouchPointer {
 public:
  // True while a finger is the pointer.
  [[nodiscard]] bool active() const noexcept { return id_ >= 0; }

  // `busy`: something else (a mouse button) holds the pointer down.
  std::optional<PointerInput> down(std::int32_t id,
                                   double x,
                                   double y,
                                   bool busy) noexcept {
    if (active() || busy || id < 0)
      return std::nullopt;
    id_ = id;
    x_ = static_cast<float>(x);
    y_ = static_cast<float>(y);
    return PointerInput{PointerInput::Action::Down, x_, y_};
  }

  std::optional<PointerInput> motion(std::int32_t id,
                                     double x,
                                     double y) noexcept {
    if (!active() || id != id_)
      return std::nullopt;
    x_ = static_cast<float>(x);
    y_ = static_cast<float>(y);
    return PointerInput{PointerInput::Action::Move, x_, y_};
  }

  // wl_touch.up carries no position: the finger lifts where it last was.
  std::optional<PointerInput> up(std::int32_t id) noexcept {
    if (!active() || id != id_)
      return std::nullopt;
    id_ = -1;
    return PointerInput{PointerInput::Action::Up, x_, y_};
  }

  // The compositor took the gesture. PointerInput has no cancel, so the
  // pointer lifts outside the surface, where no view takes it as a tap.
  std::optional<PointerInput> cancel() noexcept {
    if (!active())
      return std::nullopt;
    id_ = -1;
    return PointerInput{PointerInput::Action::Up, -1.0F, -1.0F};
  }

 private:
  std::int32_t id_ = -1;
  float x_ = 0.0F;
  float y_ = 0.0F;
};

}  // namespace acl::wm

#endif  // ACL_WM_TOUCH_POINTER_H

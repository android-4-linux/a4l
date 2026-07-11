// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §2, ADR-6, Addendum A02 (windowing via wayland-cxx-scanner).
// Android SDK ViewRootImpl/Surface docs (behavioral). No GPL-3.0 sources.
#ifndef ACLCXX_WM_PRESENT_H
#define ACLCXX_WM_PRESENT_H

// The windowing seam: present an aclcxx::gfx::Surface (a software ARGB_8888 frame
// produced by the View traversal) to a Wayland compositor. This is the narrow,
// wcs-free boundary the rest of aclcxx sees; the wcs/CRTP/xdg-shell machinery lives
// entirely in wayland_presenter.cpp and is compiled only when wcs + wayland-client
// are available (ACLCXX_HAVE_WCS). Without them, present_surface() reports the gap
// via the stub. Continuous presentation (Choreographer on wp_presentation, damage,
// multi-buffering) builds on this one-frame primitive.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>

#include "gfx/gfx.h"

namespace aclcxx::wm {

// True if a Wayland presentation backend is compiled in AND a compositor is
// reachable (WAYLAND_DISPLAY connects). Cheap; used to decide whether to present.
bool wayland_available() noexcept;

// A Choreographer-style frame producer. Called once per compositor frame tick
// (wl_surface.frame) with the frame's target time on the presentation clock
// (nanoseconds, vsync-aligned via wp_presentation feedback — the analogue of
// Choreographer.FrameCallback's frameTimeNanos) and a monotonically increasing
// index. Draw the frame into `surface`; return true to present it, false to end
// the loop (like removing the frame callback).
using FrameCallback =
    std::function<bool(gfx::Surface& surface, std::uint64_t frame_time_ns,
                       std::uint64_t frame_index)>;

// A pointer/touch input from the compositor (wl_pointer), delivered to the app as
// the native side of an android.view.MotionEvent. Coordinates are surface-local
// logical pixels. Down = button pressed, Up = released, Move = motion while down.
struct PointerInput {
  enum class Action { Down, Move, Up };
  Action action;
  float x;
  float y;
};
// A key from the compositor (wl_keyboard), delivered as the native side of an
// android.view.KeyEvent. `keycode` is the Linux evdev scancode; `keysym` is the
// xkb keysym (0 if no keymap). The app-facing Android keycode is mapped downstream.
struct KeyInput {
  enum class Action { Down, Up };
  Action action;
  std::uint32_t keycode;  // Linux evdev (KEY_A, KEY_ENTER, …)
  std::uint32_t keysym;   // xkb keysym, or 0
};
// Text from the compositor's IME (zwp_text_input_v3). `preedit` distinguishes a
// composing update (preedit_string -> InputConnection.setComposingText) from a
// final commit (commit_string -> InputConnection.commitText). `cursor` is the
// new cursor position passed to the InputConnection call.
struct TextInput {
  std::string text;
  bool preedit;
  int cursor;
};
// The IME requests deletion of text around the cursor
// (zwp_text_input_v3.delete_surrounding_text -> InputConnection.deleteSurroundingText).
struct DeleteSurroundingText {
  std::uint32_t before;  // bytes before the cursor
  std::uint32_t after;   // bytes after the cursor
};
using InputEvent = std::variant<PointerInput, KeyInput, TextInput, DeleteSurroundingText>;
// Called (on the present thread) when an input event arrives. May be empty.
using InputCallback = std::function<void(const InputEvent&)>;

// Run a continuous present loop driven by wl_surface.frame (the vsync tick) with
// wp_presentation timing feedback — the native analogue of Android's Choreographer
// posting FrameCallbacks each vsync. Double-buffered. Presents until `render`
// returns false, `max_frames` frames have been shown (0 = unbounded), or the
// toplevel closes. Returns true on a clean stop.
// `want_ime` requests text input (zwp_text_input_v3) on the surface — enabled when
// the app's focused view provides an InputConnection; committed text arrives as a
// TextInput via the input callback.
bool present_loop(int width, int height, std::string_view title,
                  std::string_view app_id, const FrameCallback& render,
                  const InputCallback& input, bool want_ime = false,
                  std::uint64_t max_frames = 0) noexcept;

// Present one frame: connect to the compositor, create an xdg_toplevel titled
// `title`/`app_id`, upload `surface`'s pixels into a wl_shm buffer, commit, and
// block until the compositor confirms the frame (wl_surface.frame callback) or
// `timeout_ms` elapses. Returns true when the frame was shown. A one-shot present
// (the surface is torn down on return) — enough to prove the seam and to snapshot.
bool present_surface(const gfx::Surface& surface, std::string_view title,
                     std::string_view app_id, int timeout_ms = 3000) noexcept;

const char* module_name() noexcept;

}  // namespace aclcxx::wm

#endif  // ACLCXX_WM_PRESENT_H

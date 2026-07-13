// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §2, ADR-6, Addendum A02 (windowing via wayland-cxx-scanner).
// Android SDK ViewRootImpl/Surface docs (behavioral). No GPL-3.0 sources.
#ifndef ACL_WM_PRESENT_H
#define ACL_WM_PRESENT_H

// The windowing seam: present an acl::gfx::Surface (a software ARGB_8888 frame
// produced by the View traversal) to a Wayland compositor. This is the narrow,
// wcs-free boundary the rest of acl sees; the wcs/CRTP/xdg-shell machinery lives
// entirely in wayland_presenter.cpp and is compiled only when wcs + wayland-client
// are available (ACL_HAVE_WCS). Without them, present_surface() reports the gap
// via the stub. Continuous presentation (Choreographer on wp_presentation, damage,
// multi-buffering) builds on this one-frame primitive.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "acl_config.h"  // ACL_HAVE_VULKAN / ACL_HAVE_WCS
#include "gfx/gfx.h"
#if ACL_HAVE_VULKAN
#include "gfx/gfx_vk.h"  // gfx::DmabufFrame
#endif

namespace acl::wm {

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

// Query the compositor's current display metrics from wl_output: the active mode's
// pixel dimensions and the integer output scale. Connects, reads one wl_output, and
// disconnects (a throwaway connection, so it can run before the app's lifecycle to
// seed android.view.Display). Returns false when no backend/compositor/wl_output is
// available, leaving the outputs untouched.
bool query_display(int& width, int& height, int& scale) noexcept;

#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
// The GPU present path (P2.4/V2b): present a Ganesh-Vulkan-rendered frame to the
// compositor as a Linux dma-buf via zwp_linux_dmabuf_v1 — the pixels reach the
// compositor without a wl_shm copy, and a scanout-capable modifier lets it promote the
// surface onto a hardware plane. Patterned after wcs's skia-vulkan-dmabuf example.

// Produce the frame to present, given the compositor's advertised DRM modifiers for the
// target format (XRGB8888). The producer picks the modifier-tiled direct path (Skia
// renders straight into the dma-buf) when one qualifies, else the linear fallback. The
// returned DmabufFrame's fd is borrowed (owned by the producer's VkRenderTarget).
using DmabufRenderFn = std::function<bool(
    const std::vector<std::uint64_t>& advertised_modifiers, gfx::DmabufFrame* out)>;

// Bind zwp_linux_dmabuf_v1, learn the compositor's advertised modifiers, call `render`
// to produce a dma-buf frame, build a wl_buffer from it (create_params → add per plane →
// create_immed), and present it under an xdg_toplevel titled `title`/`app_id`. Presents
// `frames` frames (0 = until the toplevel closes); the same frame is re-attached each
// tick for now (static present). Returns true on a clean run.
bool present_dmabuf(const DmabufRenderFn& render, std::string_view title,
                    std::string_view app_id, int frames) noexcept;

// A per-frame dma-buf producer for the animated GPU present loop (S4b): called once per
// vsync tick with the presenter-chosen `buffer_index` (which of the producer's ping-pong
// dma-buf images to render into — the presenter guarantees that image is free, i.e. the
// compositor has released it), the compositor's advertised XRGB8888 modifiers, the frame's
// vsync-aligned time (ns; Choreographer's frameTimeNanos), and a monotonically increasing
// index. Render the frame GPU-native into image `buffer_index` (begin_frame → View.draw →
// end_frame(buffer_index)) and return its descriptor in `out`. Each buffer_index maps to a
// stable fd/layout, so the presenter builds one wl_buffer per index and reuses it. Return
// false to end the loop (the analogue of removing the frame callback).
using DmabufFrameFn = std::function<bool(
    int buffer_index, const std::vector<std::uint64_t>& advertised_modifiers,
    std::uint64_t frame_time_ns, std::uint64_t frame_index, gfx::DmabufFrame* out)>;

// Run a continuous dma-buf present loop driven by wl_surface.frame (vsync), calling
// `render` each tick to produce the next GPU-native frame into a free ping-pong buffer and
// presenting it under an xdg_toplevel titled `title`/`app_id`. Double-buffered: the next
// frame renders into the buffer the compositor has released (tracked via wl_buffer.release)
// while it holds the other, so the frame-callback/release cycle keeps flowing. `input` (may
// be empty) delivers pointer/key/IME events. Presents until `render` returns false,
// `max_frames` frames are shown (0 = unbounded), or the toplevel closes. Returns true on a
// clean stop.
bool present_dmabuf_loop(int width, int height, std::string_view title,
                         std::string_view app_id, const DmabufFrameFn& render,
                         const InputCallback& input, bool want_ime = false,
                         std::uint64_t max_frames = 0) noexcept;
#endif  // ACL_HAVE_VULKAN && ACL_HAVE_WCS

const char* module_name() noexcept;

}  // namespace acl::wm

#endif  // ACL_WM_PRESENT_H

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
#include <memory>
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

// ── Looper-driven present (approach A) ──────────────────────────────────────────────
// The self-contained present_dmabuf_loop above owns its own poll() loop, so it can only
// service Wayland — it never runs the Android main Looper, and Handler-posted async work
// (Jetpack Compose's recomposer, coroutines, animations) starves. IPresenter breaks the
// loop into primitives so the *Android Looper* can own dispatch instead: the caller
// registers fd()/repeat_fd() with the MessageQueue, drives frames from the real
// Choreographer (vsync clocked by wp_presentation via frame_time_ns()), and runs the
// traversal + submit() as a CALLBACK_TRAVERSAL — ordered after Compose recomposition,
// exactly like ViewRootImpl.performTraversals. See docs/looper-driven-present.md.
//
// All methods run on the thread that called presenter_open (the app main thread); the
// object is not thread-safe. Double-buffered (ping-pong dma-buf) like the loop path.
class IPresenter {
 public:
  virtual ~IPresenter() = default;

  // The Wayland connection fd — register with the MessageQueue; call dispatch() when readable.
  virtual int fd() noexcept = 0;
  // The wl_keyboard repeat timerfd (-1 if no keyboard) — register too; dispatch_repeat() on tick.
  virtual int repeat_fd() noexcept = 0;
  // Drain queued Wayland events (input, xdg configure, wl_surface.frame, wl_buffer.release,
  // wp_presentation feedback) without blocking, then flush. Returns false once the toplevel closes.
  virtual bool dispatch() noexcept = 0;
  // Service one key-repeat tick (call when repeat_fd() is readable).
  virtual void dispatch_repeat() noexcept = 0;

  // True when the compositor is ready for the next frame: configured, a wl_surface.frame tick is
  // pending, and a ping-pong buffer is free. The driver gates vsync delivery on this.
  virtual bool ready() const noexcept = 0;
  // The free ping-pong buffer index to render into this frame (-1 if none) — pass to the GPU
  // end_frame so the produced dma-buf lands in the buffer the compositor has released.
  virtual int next_buffer() const noexcept = 0;
  // The vsync-aligned frame time (ns; Choreographer.frameTimeNanos) for the next frame.
  virtual std::uint64_t frame_time_ns() const noexcept = 0;
  // The most recent wp_presentation feedback: the real presentation-clock time the last frame lit
  // up (ns), its refresh interval (ns), and the WP_PRESENTATION_FEEDBACK_KIND_* flags. For feeding
  // TRUE present timing to an embedded client compositor (e.g. WebView's Viz, whose output device
  // otherwise reports draw-time TimeTicks::Now() with a zero interval). Returns false until a frame
  // has been presented on a CLOCK_MONOTONIC presentation clock (the base for Chromium's
  // base::TimeTicks) — i.e. no usable feedback yet, or a foreign clock. Default false (GL/stub).
  virtual bool last_present_feedback(std::uint64_t* present_ns, std::uint32_t* refresh_ns,
                                     std::uint32_t* flags) const noexcept {
    (void)present_ns; (void)refresh_ns; (void)flags; return false;
  }
  // The compositor's advertised XRGB8888 DRM modifiers (for the GPU begin_frame tiled path).
  virtual const std::vector<std::uint64_t>& modifiers() const noexcept = 0;

  // Present the produced dma-buf `frame` (rendered into next_buffer()'s image): build its
  // wl_buffer once, arm the next wl_surface.frame callback, attach + damage + commit + flush.
  // `slot` pins the ping-pong slot the frame was rendered into (from next_buffer()); pass it so the
  // wl_buffer↔dma-buf pairing stays fixed. When producer and consumer are decoupled (deferred
  // RenderThread), free_slot() at submit time can differ from the rendered slot, which — because each
  // slot's wl_buffer is bound to the fd it was first built with — would present a stale buffer
  // (flicker). -1 keeps the legacy free_slot() behavior for callers that submit in the same tick.
  virtual bool submit(const gfx::DmabufFrame& frame, int slot = -1) noexcept = 0;

  // Keep the vsync loop alive when a frame is DROPPED (the traversal threw, or the GPU could
  // not begin/end a frame, so submit() didn't run). Re-arm the next wl_surface.frame callback
  // and re-commit the last buffer without producing a new one — the last good frame stays on
  // screen and the compositor keeps ticking. Forward progress is otherwise gated entirely on
  // submit() arming frame-done, so a single dropped frame would stall the loop forever. Returns
  // false if there is nothing to keep alive yet (no frame ever committed) or the toplevel closed.
  virtual bool keepalive() noexcept = 0;

  // True once the compositor/user has closed the toplevel.
  virtual bool closed() const noexcept = 0;

  // ── GL mode (Mesa owns buffers/commit) ──────────────────────────────────────────────
  // For a native-GL app (Flutter), Mesa renders + commits the toplevel wl_surface itself (inside
  // eglSwapBuffers), so acl allocates no dma-buf and never calls submit(). acl still owns the
  // Looper (fd()/dispatch()) and the vsync clock: it hands out the raw wl_display + wl_surface to
  // build the GL device on, arms wp_presentation feedback for Mesa's commits (so frame_time_ns()
  // tracks them), and waits for the first xdg configure before the GL device is created. Defaults
  // are the non-GL behavior.
  virtual void* gl_display() noexcept { return nullptr; }  // raw wl_display* for the GlDevice
  virtual void* gl_surface() noexcept { return nullptr; }  // toplevel wl_surface* Mesa renders into
  virtual void arm_present() noexcept {}                   // request wp_presentation feedback for the next commit
  virtual bool wait_configured(int timeout_ms) noexcept { return true; }  // pump until first configure
};

// Connect to the compositor and create a titled xdg_toplevel, returning a presenter ready for
// its first frame (configured, one buffer free). `input` (may be empty) receives pointer/key/IME.
// Returns nullptr when no backend/compositor is available. Destroy to tear down + disconnect.
std::unique_ptr<IPresenter> presenter_open(int width, int height, std::string_view title,
                                           std::string_view app_id, InputCallback input,
                                           bool want_ime = false) noexcept;
#endif  // ACL_HAVE_VULKAN && ACL_HAVE_WCS

#if ACL_HAVE_WCS
// ── Externally-rendered (GL / Mesa-owned) present ───────────────────────────────────
// The GLSurfaceView path: an app's GLThread renders with host Mesa (EGL_PLATFORM_WAYLAND)
// straight into the toplevel's wl_surface, so Mesa — not acl — owns buffer allocation,
// attach, commit and the vsync throttle (all inside eglSwapBuffers). wm/ only creates the
// xdg_toplevel role and dispatches events, handing the caller the raw wl_display + wl_surface
// to build its GL device/window on. This keeps wm/ free of any EGL/GL types.
struct GlPresentHooks {
  // Called once after the first xdg configure, with acl's wl_display, the toplevel
  // wl_surface, and the logical size — build the GlDevice/GlWindow here. False aborts.
  std::function<bool(void* wl_display, void* wl_surface, int width, int height)> configure;
  // Called each loop iteration with a monotonically increasing frame index: draw the GL
  // frame and eglSwapBuffers (which commits + throttles to vsync). False stops the loop.
  std::function<bool(std::uint64_t frame_index)> render;
  // Called on a compositor resize (xdg toplevel configure) with the new logical size (may
  // be empty). The caller resizes its wl_egl_window.
  std::function<void(int width, int height)> resize;
  // Called once when the loop ends, BEFORE acl disconnects the wl_display (may be empty).
  // The caller MUST destroy its GL device/window here: they are EGL objects built on this
  // wl_display, so tearing them down after the display is gone is a use-after-free.
  std::function<void()> teardown;
};

// Set up a titled xdg_toplevel and run an externally-rendered present loop: after the first
// configure, call hooks.configure so the caller can bind a Mesa GL window to the surface,
// then each iteration call hooks.render (GL draw + swap) while dispatching input and xdg
// events. Presents until render returns false, `max_frames` frames are shown (0 = unbounded),
// or the toplevel closes. `input` (may be empty) delivers pointer/key/IME. Clean stop -> true.
bool present_gl_loop(int width, int height, std::string_view title, std::string_view app_id,
                     const GlPresentHooks& hooks, const InputCallback& input,
                     std::uint64_t max_frames = 0) noexcept;
#endif  // ACL_HAVE_WCS

const char* module_name() noexcept;

}  // namespace acl::wm

#endif  // ACL_WM_PRESENT_H

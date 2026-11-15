// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (docs/adr/ADR-0009, docs/split-process-art.md
// S2/S3/S5). No GPL-3.0 sources.
//
// The looper-driven present path across a process boundary.
//
// The app process (the child) renders into dma-bufs exactly as it does
// single-process, but its IPresenter is a proxy: submit() sends the dma-buf
// fd over acl_ipc, and input, vsync and buffer state arrive the same way. The
// broker process (the parent) owns the only Wayland connection: it opens the
// real presenter, imports each frame, and forwards input.
//
// Covered: the Skia dma-buf path (ACL_PRESENT_GPU), and the video plane: a
// decoded frame is sent as its own dma-buf and the broker puts it on the
// subsurface below the window. Native GL crosses by other means
// (runtime/jvm_vm.cpp: a gbm surface in the child, its frames sent here).
#ifndef ACL_WM_SPLIT_PRESENT_H
#define ACL_WM_SPLIT_PRESENT_H

#include <functional>
#include <memory>
#include <string_view>

#include "acl_config.h"
#include "ipc/ipc.h"
#include "wm/present.h"
#include "wm/split_validate.h"

namespace acl::wm {

// ── Child ────────────────────────────────────────────────────────────────

// Make this process a split child: every presenter it opens is a proxy over
// `ipc_fd`, and query_display() reports the size given here. Call once, before
// the VM starts. Takes ownership of the fd.
void split_child_install(int ipc_fd, int display_w, int display_h, int scale);
bool split_child_active() noexcept;
// The display the broker reported. False if this is not a split child.
bool split_child_display(int& width, int& height, int& scale) noexcept;

#if ACL_HAVE_VULKAN && ACL_HAVE_WCS

// The proxy presenter. Null if this is not a split child or the broker could
// not create the surface.
std::unique_ptr<IPresenter> split_presenter_open(int width,
                                                 int height,
                                                 std::string_view title,
                                                 std::string_view app_id,
                                                 InputCallback input,
                                                 bool want_ime) noexcept;
// The same proxy over a channel the caller owns and keeps alive. What
// split_presenter_open() calls with the installed channel; also the test seam.
std::unique_ptr<IPresenter> split_presenter_open_on(ipc::Channel& channel,
                                                    int width,
                                                    int height,
                                                    std::string_view title,
                                                    std::string_view app_id,
                                                    InputCallback input,
                                                    bool want_ime) noexcept;

// ── Broker ───────────────────────────────────────────────────────────────

using PresenterFactory =
    std::function<std::unique_ptr<IPresenter>(int width,
                                              int height,
                                              std::string_view title,
                                              std::string_view app_id,
                                              InputCallback input,
                                              bool want_ime)>;

struct BrokerStats {
  std::uint64_t frames = 0;    // PresentFrame messages submitted
  std::uint64_t rejected = 0;  // messages refused by validation
  // Of those, refused frames by reason, indexed by FrameVerdict.
  std::uint64_t frame_verdicts[kFrameVerdictCount] = {};
  std::uint64_t video_frames = 0;  // VideoFrame messages given to the plane
  std::uint64_t inputs = 0;        // input events forwarded
  std::uint64_t from_child = 0;    // datagrams received
  std::uint64_t to_child = 0;      // datagrams sent
  bool window_closed = false;      // the toplevel closed before the child left
};

// What the broker decides for the child and does not take from it.
struct BrokerOptions {
  // The window's app id. It names the app to the compositor (icon, window
  // rules, task switcher), so it comes from the APK the broker admitted, not
  // from the process running it. Empty: use what the child sends, cleaned.
  std::string app_id;
  // How a received descriptor is examined. inspect_buffer outside tests,
  // which have no dma-buf to send.
  std::function<BufferFacts(int fd)> inspect = inspect_buffer;
  // Surfaces the child may create over the connection's life. Each is a new
  // toplevel on the user's screen.
  int max_surfaces = 16;
  // Messages handled before the broker returns to the compositor's events,
  // so that a child that floods the socket cannot starve the window.
  int drain_budget = 64;
};

// Serve one child over `channel` until it disconnects or the window closes.
// `open` creates the real presenter (wm::presenter_open outside tests). When
// the window closes the child is told and given `linger_ms` to disconnect.
// Everything the child sends is validated before it reaches the presenter
// (wm/split_validate.h); a message that fails is dropped and counted.
BrokerStats serve_split_child(ipc::Channel& channel,
                              const PresenterFactory& open,
                              const BrokerOptions& options = {},
                              int linger_ms = 3000) noexcept;

#endif  // ACL_HAVE_VULKAN && ACL_HAVE_WCS

}  // namespace acl::wm

#endif  // ACL_WM_SPLIT_PRESENT_H

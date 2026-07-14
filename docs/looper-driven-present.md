<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: acl P2.4 display-pipeline design; Android ViewRootImpl/Choreographer/
     DisplayEventReceiver behavior (AOSP docs) + wp_presentation/zwp_linux_dmabuf (Wayland).
     No GPL-3.0 sources. -->

# Looper-driven present (approach A)

## Problem

acl's GPU present (`present_dmabuf_loop`) is a **hand-rolled `poll()` loop** on the JVM main
thread. It polls only the Wayland fd; it never runs the Android main `Looper`. But Android's
async framework — most sharply **Jetpack Compose's recomposer** — posts its continuations to the
main `Handler` queue. With that queue never drained, Compose's first composition never completes,
and `View.measure` blocks on it (verified: the thread is a blocked wait, not a spin). Manually
draining the queue once per frame is a band-aid that fails because the composition suspends across
the frame-clock hand-off.

## Design

**One event loop — the Android `Looper` — owns dispatch on the main thread**, servicing both
Wayland events *and* Handler messages, exactly as a real Android UI thread does. Frames come from
the real `Choreographer`, clocked by `wp_presentation` feedback (the true compositor vsync), and
the traversal runs as a `CALLBACK_TRAVERSAL` — ordered **after** Compose's recomposition
(`CALLBACK_ANIMATION`), like `ViewRootImpl.performTraversals`.

```
Looper.loop()  (main thread)
 ├─ MessageQueue.addOnFileDescriptorEventListener(wl_fd) ─→ presenter.dispatch()
 │      dispatches wl events: input, xdg configure, wl_surface.frame, wl_buffer.release,
 │      wp_presentation feedback.  When the compositor is ready (frame cb + free slot),
 │      deliver a vsync → DisplayEventReceiver.dispatchVsync(frameTimeNanos)
 │
 ├─ Choreographer.doFrame(frameTimeNanos)   (fired by the delivered vsync)
 │      CALLBACK_ANIMATION  → Compose recomposition (completes: Handler is being run)
 │      CALLBACK_TRAVERSAL  → acl traversal: beginFrame → measure → layout → View.draw
 │                             → endFrame(dma-buf) → presenter.submit (attach+damage+commit,
 │                             arm next wl_surface.frame) → re-post the traversal
 │
 └─ Handler messages (Compose continuations, coroutines, animations) run between frames.
```

Vsync clock: `presenter.frame_time_ns()` = last `wp_presentation` present + one refresh interval
(the analogue of `Choreographer.frameTimeNanos`). No synthetic timer — the compositor paces us.

## Components

1. **`wm::IPresenter` (present.h) + Presenter primitives (dmabuf_presenter.cpp)** — refactor the
   self-contained loop into: `open` (connect + titled xdg_toplevel), `fd`, `dispatch` (non-blocking
   drain + flush), `ready` (configured + free ping-pong slot + frame-cb pending), `frame_time_ns`,
   `next_buffer` (free slot to render into), `submit(DmabufFrame)` (build wl_buffer + attach +
   damage + commit + arm frame cb), `closed`, `close`. The old `present_dmabuf_loop` stays for the
   synthetic-View demos.

2. **DisplayEventReceiver vsync registry (graphics_jni.cpp)** — `nativeScheduleVsync` records a
   *pending* request per live receiver (no inline dispatch). Export `acl_deliver_vsync(frameTimeNanos)`
   that dispatches the pending vsync to the registered receivers → `Choreographer.doFrame`. The
   present driver calls it when `presenter.ready()`.

3. **Native present-driver helpers (svc/present or jvm_vm)** — thin JNI over the Presenter +
   the GPU begin/end frame + `acl_deliver_vsync`, so the Java driver can open/dispatch/begin/
   submit/close.

4. **`org.acl.internal.PresentDriver` (Java)** — registers the wl fd with the MessageQueue, posts
   the traversal as `CALLBACK_TRAVERSAL` (re-posted per frame), delivers vsync on compositor
   readiness, runs `Looper.loop()`. Input events (pointer/key/IME) from the presenter dispatch to
   the focused View.

5. **jvm_vm wiring** — `render_content_view` (GPU present path) hands the decor View to
   `PresentDriver` instead of the native `present_dmabuf_loop`.

## Why this is the foundation, not a workaround

It is how Android itself runs a UI thread: one Looper, the Choreographer on real vsync, traversal
after composition. Every async framework subsystem (Compose, coroutines, animations, Handlers,
`postOnAnimation`, IME) is serviced correctly for free — not just the one Compose banner. The
synthetic-vsync `DisplayEventReceiver` becomes wp_presentation-clocked; input, IME, and multi-buffer
damage already live in the presenter.

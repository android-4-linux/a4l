// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// Choreographer): the postFrameCallback / FrameCallback surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.Choreographer (Phase 2). Real
// Choreographer schedules callbacks on the display vsync via the native
// DisplayEventReceiver; here the acl ViewRoot driver (jvm_vm) pumps the queue
// once per Wayland frame tick (wl_surface.frame, aligned by wp_presentation) by
// calling aclDoFrame(frameTimeNanos). Callbacks are one-shot — an animation
// re-posts itself from doFrame(), exactly as on Android. Docs-derived shim, NOT
// vendored AOSP.
package android.view;

import java.util.ArrayList;

public final class Choreographer {
  public interface FrameCallback {
    void doFrame(long frameTimeNanos);
  }

  private static final Choreographer INSTANCE = new Choreographer();

  public static Choreographer getInstance() { return INSTANCE; }

  private ArrayList<FrameCallback> pending = new ArrayList<FrameCallback>();

  public void postFrameCallback(FrameCallback callback) {
    if (callback != null) pending.add(callback);
  }
  public void removeFrameCallback(FrameCallback callback) {
    pending.remove(callback);
  }

  // Fire and clear the queued callbacks for one frame (the acl vsync tick).
  // Swaps the queue first so a callback that re-posts itself lands in the NEXT
  // frame, not this one. Returns how many callbacks fired (0 => nothing animating).
  public long aclDoFrame(long frameTimeNanos) {
    ArrayList<FrameCallback> now = pending;
    pending = new ArrayList<FrameCallback>();
    for (int i = 0; i < now.size(); i++) {
      now.get(i).doFrame(frameTimeNanos);
    }
    return now.size();
  }
}

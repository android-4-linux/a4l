// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// View): the draw/onDraw subset. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.View (Phase 2). A custom view
// overrides onDraw(Canvas); the framework calls draw(). Real View(Context) +
// measure/layout/the ViewRoot traversal + Wayland presentation arrive with the
// full window stack; this exists so an app's onDraw -> Canvas -> gfx path runs.
// Docs-derived shim, NOT vendored AOSP.
package android.view;

import android.graphics.Canvas;

public class View {
  public View() {}

  public void draw(Canvas canvas) { onDraw(canvas); }
  protected void onDraw(Canvas canvas) {}
  public void invalidate() {}
}

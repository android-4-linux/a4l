// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/graphics/
// Paint): the color-holding subset. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.graphics.Paint (Phase 2). Stroke/
// style/typeface (the Skia+text stack) arrive with real rendering. Docs-derived
// shim, NOT vendored AOSP.
package android.graphics;

public class Paint {
  private int color = 0xFF000000;

  public Paint() {}

  public void setColor(int color) { this.color = color; }
  public int getColor() { return color; }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/graphics/
// Canvas): the fill/drawRect subset. Native underside -> aclcxx::gfx::Surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.graphics.Canvas (Phase 2). Draws
// into the gfx::Surface a Bitmap wraps; drawText/drawPath/etc. arrive with the
// Skia + text stack. Docs-derived shim, NOT vendored AOSP.
package android.graphics;

public class Canvas {
  private final long nativePtr;

  public Canvas(Bitmap bitmap) { this.nativePtr = bitmap.aclcxxPtr(); }

  public void drawColor(int color) { nativeDrawColor(nativePtr, color); }
  public void drawRGB(int r, int g, int b) {
    nativeDrawColor(nativePtr, 0xFF000000 | (r << 16) | (g << 8) | b);
  }
  public void drawRect(float left, float top, float right, float bottom, Paint paint) {
    nativeDrawRect(nativePtr, left, top, right, bottom, paint.getColor());
  }

  private static native void nativeDrawColor(long ptr, int color);
  private static native void nativeDrawRect(long ptr, float left, float top,
                                            float right, float bottom, int color);
}

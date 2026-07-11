// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/graphics/
// Canvas): the fill/drawRect + save/translate/restore subset. Native underside ->
// acl::gfx::Surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.graphics.Canvas (Phase 2). Draws
// into the gfx::Surface a Bitmap wraps; drawText/drawPath/etc. arrive with the
// Skia + text stack. The current-matrix is a translate-only offset stack (enough
// for ViewGroup.dispatchDraw to place children); the full Skia matrix/clip stack
// arrives with real rendering. Docs-derived shim, NOT vendored AOSP.
package android.graphics;

public class Canvas {
  private final long nativePtr;
  // Translate-only current transform + a save/restore stack of it.
  private float dx, dy;
  private final float[] savedX = new float[32];
  private final float[] savedY = new float[32];
  private int sp;

  public Canvas(Bitmap bitmap) { this.nativePtr = bitmap.aclPtr(); }

  public int save() {
    savedX[sp] = dx;
    savedY[sp] = dy;
    sp++;
    return sp;
  }
  public void restore() {
    sp--;
    dx = savedX[sp];
    dy = savedY[sp];
  }
  public void translate(float x, float y) {
    dx += x;
    dy += y;
  }

  public void drawColor(int color) { nativeDrawColor(nativePtr, color); }
  public void drawRGB(int r, int g, int b) {
    nativeDrawColor(nativePtr, 0xFF000000 | (r << 16) | (g << 8) | b);
  }
  public void drawRect(float left, float top, float right, float bottom, Paint paint) {
    nativeDrawRect(nativePtr, left + dx, top + dy, right + dx, bottom + dy,
                   paint.getColor());
  }

  private static native void nativeDrawColor(long ptr, int color);
  private static native void nativeDrawRect(long ptr, float left, float top,
                                            float right, float bottom, int color);
}

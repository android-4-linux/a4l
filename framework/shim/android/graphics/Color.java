// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/graphics/
// Color): public constants + argb/rgb. Pure Java (no native). No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.graphics.Color (Gate G2/Phase 2).
package android.graphics;

public class Color {
  public static final int BLACK = 0xFF000000;
  public static final int WHITE = 0xFFFFFFFF;
  public static final int RED = 0xFFFF0000;
  public static final int GREEN = 0xFF00FF00;
  public static final int BLUE = 0xFF0000FF;
  public static final int TRANSPARENT = 0x00000000;

  public static int argb(int a, int r, int g, int b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
  }
  public static int rgb(int r, int g, int b) {
    return 0xFF000000 | (r << 16) | (g << 8) | b;
  }
}

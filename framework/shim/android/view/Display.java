// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// Display): public API surface. Native underside -> svc/ WindowService. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.Display (svc hub). Returned by
// WindowManager.getDefaultDisplay(); reports the logical display metrics until the
// wm/ + gfx/ Wayland/Skia stack lands (Phase 2). Docs-derived shim, NOT vendored.
package android.view;

public class Display {
  public static final int DEFAULT_DISPLAY = 0;

  public Display() {}

  public int getWidth() { return nativeGetWidth(); }
  public int getHeight() { return nativeGetHeight(); }
  public int getRotation() { return nativeGetRotation(); }
  public int getDisplayId() { return DEFAULT_DISPLAY; }

  private static native int nativeGetWidth();
  private static native int nativeGetHeight();
  private static native int nativeGetRotation();
}

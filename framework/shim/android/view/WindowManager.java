// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// WindowManager): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.WindowManager (svc hub).
// Returned by Context.getSystemService(WINDOW_SERVICE); getDefaultDisplay() yields
// the logical Display. addView/removeView (the View stack) arrive in Phase 2. Real
// WindowManager is an interface; this is a concrete shim. Docs-derived, NOT vendored.
package android.view;

public class WindowManager {
  public WindowManager() {}

  public Display getDefaultDisplay() { return new Display(); }
}

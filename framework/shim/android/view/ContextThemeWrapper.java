// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// ContextThemeWrapper): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.ContextThemeWrapper — the
// theme-carrying ContextWrapper that Activity extends. Theme/Resources handling
// arrives with the View stack (Phase 2); this exists so the Activity -> Context
// type hierarchy is real. Docs-derived shim, NOT vendored AOSP.
package android.view;

import android.content.Context;
import android.content.ContextWrapper;

public class ContextThemeWrapper extends ContextWrapper {
  public ContextThemeWrapper() {}
  public ContextThemeWrapper(Context base, int themeResId) { super(base); }
}

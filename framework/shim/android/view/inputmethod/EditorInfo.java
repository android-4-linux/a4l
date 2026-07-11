// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// inputmethod/EditorInfo). No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.inputmethod.EditorInfo (the
// editor attributes passed to View.onCreateInputConnection). Docs-derived shim.
package android.view.inputmethod;

public class EditorInfo {
  public int inputType;
  public int imeOptions;
  public EditorInfo() {}
}

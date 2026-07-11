// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// inputmethod/InputConnection): the commitText subset. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.inputmethod.InputConnection
// (Phase 2 IME). The real interface is the ~20-method channel between an IME and
// an editable view; here it carries the one method the aclcxx ViewRoot driver
// calls when the compositor's IME (zwp_text_input_v3) commits text. Docs-derived
// shim, NOT vendored AOSP.
package android.view.inputmethod;

public interface InputConnection {
  boolean commitText(CharSequence text, int newCursorPosition);
  // Composing (preedit) text from the IME, updated as the user types before commit.
  boolean setComposingText(CharSequence text, int newCursorPosition);
}

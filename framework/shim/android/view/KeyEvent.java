// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// KeyEvent): the action/keycode subset. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.KeyEvent (Phase 2 input). Real
// KeyEvent carries scancodes, meta/modifier state, repeat counts and Unicode
// composition; here the acl ViewRoot driver builds a single key event per
// wl_keyboard key (state->ACTION_DOWN/UP, evdev->Android keycode) via obtain().
// Docs-derived shim, NOT vendored AOSP.
package android.view;

public class KeyEvent {
  public static final int ACTION_DOWN = 0;
  public static final int ACTION_UP = 1;

  // A small subset of the documented keycodes (enough for apps/tests to match).
  public static final int KEYCODE_UNKNOWN = 0;
  public static final int KEYCODE_0 = 7;
  public static final int KEYCODE_A = 29;
  public static final int KEYCODE_DPAD_UP = 19;
  public static final int KEYCODE_DPAD_DOWN = 20;
  public static final int KEYCODE_DPAD_LEFT = 21;
  public static final int KEYCODE_DPAD_RIGHT = 22;
  public static final int KEYCODE_DEL = 67;
  public static final int KEYCODE_TAB = 61;
  public static final int KEYCODE_SPACE = 62;
  public static final int KEYCODE_ENTER = 66;
  public static final int KEYCODE_ESCAPE = 111;

  private final int action;
  private final int keyCode;

  public KeyEvent(int action, int code) {
    this.action = action;
    this.keyCode = code;
  }

  // The acl ViewRoot driver's factory (matches the native (II)-> signature).
  public static KeyEvent obtain(int action, int keyCode) {
    return new KeyEvent(action, keyCode);
  }

  public int getAction() { return action; }
  public int getKeyCode() { return keyCode; }
}

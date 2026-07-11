// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// MotionEvent): the obtain/getAction/getX/getY subset. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.MotionEvent (Phase 2 input).
// Real MotionEvent carries a batched, historical, multi-pointer sample stream from
// the InputManager; here the acl ViewRoot driver builds a single-pointer event
// per wl_pointer input (button->DOWN/UP, motion->MOVE) via obtain(). Docs-derived
// shim, NOT vendored AOSP.
package android.view;

public final class MotionEvent {
  public static final int ACTION_DOWN = 0;
  public static final int ACTION_UP = 1;
  public static final int ACTION_MOVE = 2;
  public static final int ACTION_CANCEL = 3;
  public static final int ACTION_MASK = 0xff;

  private long downTime;
  private long eventTime;
  private int action;
  private float x;
  private float y;
  private int metaState;

  private MotionEvent() {}

  public static MotionEvent obtain(long downTime, long eventTime, int action,
                                   float x, float y, int metaState) {
    MotionEvent e = new MotionEvent();
    e.downTime = downTime;
    e.eventTime = eventTime;
    e.action = action;
    e.x = x;
    e.y = y;
    e.metaState = metaState;
    return e;
  }

  public int getAction() { return action; }
  public int getActionMasked() { return action & ACTION_MASK; }
  public float getX() { return x; }
  public float getY() { return y; }
  public long getDownTime() { return downTime; }
  public long getEventTime() { return eventTime; }
  public int getMetaState() { return metaState; }

  // Shift the reported location — used by ViewGroup.dispatchTouchEvent to convert
  // to a child's local coordinate space.
  public void offsetLocation(float dx, float dy) {
    x += dx;
    y += dy;
  }

  public void recycle() {}  // no pool yet; kept for source compatibility
}

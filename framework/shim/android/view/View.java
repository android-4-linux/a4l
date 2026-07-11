// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// View): the constructor, MeasureSpec, and the measure/layout/draw traversal
// surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.View (Phase 2). It carries the
// real public traversal contract an app relies on — View(Context), MeasureSpec, the
// measure -> onMeasure -> setMeasuredDimension pass, layout -> onLayout, and draw ->
// onDraw -> dispatchDraw — so a custom view's overrides run under the aclcxx
// ViewRoot driver (jvm_vm render_content_view, standing in for ViewRootImpl.
// performTraversals). The full ViewRootImpl/measure-cache/attach-info machinery and
// Wayland presentation arrive with the window stack; this is the docs-derived
// subset, NOT vendored AOSP.
package android.view;

import android.content.Context;
import android.graphics.Canvas;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

public class View {
  private Context mContext;
  private int mLeft, mTop, mRight, mBottom;
  private int mMeasuredWidth, mMeasuredHeight;
  private int mMinWidth, mMinHeight;

  public View() {}
  public View(Context context) { mContext = context; }

  public final Context getContext() { return mContext; }

  // MeasureSpec: a size packed with a mode in the top two bits (the app-visible
  // contract onMeasure implementations decode).
  public static class MeasureSpec {
    private static final int MODE_SHIFT = 30;
    private static final int MODE_MASK = 0x3 << MODE_SHIFT;
    public static final int UNSPECIFIED = 0 << MODE_SHIFT;
    public static final int EXACTLY = 1 << MODE_SHIFT;
    public static final int AT_MOST = 2 << MODE_SHIFT;

    public static int makeMeasureSpec(int size, int mode) {
      return (size & ~MODE_MASK) | (mode & MODE_MASK);
    }
    public static int getMode(int measureSpec) { return measureSpec & MODE_MASK; }
    public static int getSize(int measureSpec) { return measureSpec & ~MODE_MASK; }
  }

  // measure -> onMeasure (which must setMeasuredDimension). The default honors an
  // EXACTLY/AT_MOST spec size and otherwise reports the suggested minimum.
  public final void measure(int widthMeasureSpec, int heightMeasureSpec) {
    onMeasure(widthMeasureSpec, heightMeasureSpec);
  }
  protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
    setMeasuredDimension(getDefaultSize(getSuggestedMinimumWidth(), widthMeasureSpec),
                         getDefaultSize(getSuggestedMinimumHeight(), heightMeasureSpec));
  }
  protected final void setMeasuredDimension(int measuredWidth, int measuredHeight) {
    mMeasuredWidth = measuredWidth;
    mMeasuredHeight = measuredHeight;
  }
  public final int getMeasuredWidth() { return mMeasuredWidth; }
  public final int getMeasuredHeight() { return mMeasuredHeight; }

  public static int getDefaultSize(int size, int measureSpec) {
    int result = size;
    int specMode = MeasureSpec.getMode(measureSpec);
    int specSize = MeasureSpec.getSize(measureSpec);
    if (specMode == MeasureSpec.AT_MOST || specMode == MeasureSpec.EXACTLY) {
      result = specSize;
    }
    return result;
  }
  protected int getSuggestedMinimumWidth() { return mMinWidth; }
  protected int getSuggestedMinimumHeight() { return mMinHeight; }
  public void setMinimumWidth(int minWidth) { mMinWidth = minWidth; }
  public void setMinimumHeight(int minHeight) { mMinHeight = minHeight; }

  // layout assigns the frame and hands the positioning of any children to onLayout.
  public void layout(int l, int t, int r, int b) {
    boolean changed = l != mLeft || t != mTop || r != mRight || b != mBottom;
    mLeft = l; mTop = t; mRight = r; mBottom = b;
    onLayout(changed, l, t, r, b);
  }
  protected void onLayout(boolean changed, int l, int t, int r, int b) {}

  public final int getLeft() { return mLeft; }
  public final int getTop() { return mTop; }
  public final int getRight() { return mRight; }
  public final int getBottom() { return mBottom; }
  public final int getWidth() { return mRight - mLeft; }
  public final int getHeight() { return mBottom - mTop; }

  // draw -> onDraw (this view) -> dispatchDraw (children). A leaf overrides onDraw;
  // a container overrides dispatchDraw (see ViewGroup).
  public void draw(Canvas canvas) {
    onDraw(canvas);
    dispatchDraw(canvas);
  }
  protected void onDraw(Canvas canvas) {}
  protected void dispatchDraw(Canvas canvas) {}

  public void invalidate() {}

  // Input: the ViewRoot driver delivers a pointer as a MotionEvent here; a leaf
  // view handles it in onTouchEvent. Real dispatch hit-tests and walks the tree
  // (ViewGroup.dispatchTouchEvent) and includes gesture/focus machinery — that
  // arrives with the input pipeline; this is the leaf contract an app overrides.
  public boolean dispatchTouchEvent(MotionEvent event) {
    return onTouchEvent(event);
  }
  public boolean onTouchEvent(MotionEvent event) {
    return false;
  }

  // Keyboard: the driver delivers a KeyEvent here; dispatch routes it to the
  // action-specific hooks a view overrides. Real dispatch runs through the focus
  // chain / IME; this is the leaf contract.
  public boolean dispatchKeyEvent(KeyEvent event) {
    if (event.getAction() == KeyEvent.ACTION_DOWN) {
      return onKeyDown(event.getKeyCode(), event);
    }
    return onKeyUp(event.getKeyCode(), event);
  }
  public boolean onKeyDown(int keyCode, KeyEvent event) {
    return false;
  }
  public boolean onKeyUp(int keyCode, KeyEvent event) {
    return false;
  }

  // IME: an editable view returns an InputConnection here; the aclcxx ViewRoot
  // driver enables Wayland text input when it does, and routes committed text to
  // it. Non-editable views return null (no soft input).
  public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
    return null;
  }
}

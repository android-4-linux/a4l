// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/view/
// ViewGroup): the child-management + measure/dispatchDraw subset. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.view.ViewGroup (Phase 2): holds a
// child list, measures each child, and draws them under a translated Canvas. Real
// ViewGroup.onLayout is abstract (each layout manager positions its children); here
// it is a no-op default so a subclass (or a widget layout) provides positioning.
// The LayoutParams/measure-cache/touch-dispatch machinery arrives with the widget
// set; this is the docs-derived subset an app's custom container needs, NOT
// vendored AOSP.
package android.view;

import android.content.Context;
import android.graphics.Canvas;

import java.util.ArrayList;

public class ViewGroup extends View {
  private final ArrayList<View> children = new ArrayList<View>();

  public ViewGroup(Context context) { super(context); }

  public void addView(View child) { children.add(child); }
  public int getChildCount() { return children.size(); }
  public View getChildAt(int index) { return children.get(index); }

  // Measure every child against the container's spec, then adopt the spec size.
  @Override
  protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
    for (int i = 0; i < children.size(); i++) {
      children.get(i).measure(widthMeasureSpec, heightMeasureSpec);
    }
    setMeasuredDimension(getDefaultSize(0, widthMeasureSpec),
                         getDefaultSize(0, heightMeasureSpec));
  }

  // Draw each child in its own coordinate space (translate by its laid-out origin).
  @Override
  protected void dispatchDraw(Canvas canvas) {
    for (int i = 0; i < children.size(); i++) {
      View child = children.get(i);
      canvas.save();
      canvas.translate(child.getLeft(), child.getTop());
      child.draw(canvas);
      canvas.restore();
    }
  }

  @Override
  protected void onLayout(boolean changed, int l, int t, int r, int b) {}
}

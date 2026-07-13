// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK android.view.ViewGroup / android.view.View
// (public API). No GPL-3.0 sources.
//
// A real ViewGroup hierarchy: three vertically-stacked Tile children. Exercises the real
// framework View traversal on host ART — ViewGroup.onMeasure/onLayout + dispatchDraw drawing
// each child under a Canvas transform (save/translate/restore) — presented GPU-native.
package com.acl.hello;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;

public class Panel extends ViewGroup {
    public Panel(Context c) {
        super(c);
        addView(new Tile(c, 0xFF204060, "one"));
        addView(new Tile(c, 0xFFCC8844, "two"));
        addView(new Tile(c, 0xFF308060, "three"));
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        final int w = MeasureSpec.getSize(widthSpec);
        final int h = MeasureSpec.getSize(heightSpec);
        final int n = getChildCount();
        final int ch = n > 0 ? h / n : h;
        for (int i = 0; i < n; i++) {
            getChildAt(i).measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
                                  MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY));
        }
        setMeasuredDimension(w, h);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        final int n = getChildCount();
        final int ch = n > 0 ? (b - t) / n : (b - t);
        for (int i = 0; i < n; i++) {
            getChildAt(i).layout(0, i * ch, r - l, (i + 1) * ch);
        }
    }
}

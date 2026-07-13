// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK android.view.View / android.graphics
// (public API). No GPL-3.0 sources.
//
// A leaf View: fills its box with a color and draws a label. Child of Panel — exercises
// the real ViewGroup traversal (each child drawn under a Canvas translate to its position).
package com.acl.hello;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.View;

public class Tile extends View {
    private final int color;
    private final String label;
    private final Paint paint = new Paint();

    public Tile(Context c, int color, String label) {
        super(c);
        this.color = color;
        this.label = label;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawColor(color);
        paint.setColor(0xFFFFFFFF);
        paint.setAntiAlias(true);
        paint.setTextSize(getHeight() / 3f);
        canvas.drawText(label, 16, getHeight() / 2f + getHeight() / 8f, paint);
    }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK android.view.View / android.graphics
// (public API). No GPL-3.0 sources.
//
// A real android.view.View subclass whose onDraw exercises the acl-owned android.graphics
// natives (drawColor/drawCircle/drawText/gradient) on host ART. Used to drive the real
// View traversal (measure/layout/draw) through the real framework View + Canvas — the
// "app path" beginning. Compiled against android.jar; runs against the real framework jars.
package com.acl.hello;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.view.View;

public class DrawView extends View {
    private final Paint paint = new Paint();

    public DrawView(Context context) {
        super(context);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final int w = getWidth(), h = getHeight();
        // A vertical blue->teal gradient background.
        paint.setShader(new LinearGradient(0, 0, 0, h, 0xFF102040, 0xFF108080,
                Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, paint);
        paint.setShader(null);
        // An orange filled circle in the center.
        paint.setColor(0xFFCC8844);
        paint.setAntiAlias(true);
        canvas.drawCircle(w / 2f, h / 2f, Math.min(w, h) / 4f, paint);
        // A white label.
        paint.setColor(0xFFFFFFFF);
        paint.setTextSize(h / 8f);
        canvas.drawText("acl", w / 2f - w / 8f, h / 2f, paint);
    }
}

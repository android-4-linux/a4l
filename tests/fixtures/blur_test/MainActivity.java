// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK View.setRenderEffect / RenderEffect.createBlur
// / Shader.TileMode docs. No GPL-3.0 sources.
//
// Minimum-viable repro of RenderNode RenderEffect blur (Compose Modifier.blur / View.setRenderEffect).
// A centerd View draws a SHARP vertical red|blue split, then has a blur RenderEffect applied. Correct
// render: the once-1px-sharp seam becomes a wide purple-ish gradient (red and blue mixed) — proof the
// node's content is composited through the SkImageFilter blur. Broken (no effect): a crisp seam.
package com.acl.blur;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RenderEffect;
import android.graphics.Shader;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;

public class MainActivity extends Activity {
    static class SplitView extends View {
        final Paint p = new Paint();
        SplitView(Context c) { super(c); }
        @Override protected void onDraw(Canvas canvas) {
            int w = getWidth(), h = getHeight();
            p.setColor(Color.rgb(220, 40, 40)); canvas.drawRect(0, 0, w / 2f, h, p);       // left red
            p.setColor(Color.rgb(40, 90, 220)); canvas.drawRect(w / 2f, 0, w, h, p);       // right blue
        }
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);

        SplitView v = new SplitView(this);
        v.setRenderEffect(RenderEffect.createBlurEffect(40f, 40f, Shader.TileMode.CLAMP));
        root.addView(v, new FrameLayout.LayoutParams(600, 600, Gravity.CENTER));

        setContentView(root);
    }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (View.setLayerType,
// RenderNode.setUseCompositingLayer, PorterDuff.Mode). No GPL-3.0 sources.
//
// Views that are drawn into a compositing layer of their own, over a blue window. A layer
// holds only what its view draws, so a blend mode that reads the destination works on the
// view's own pixels and leaves the window under it alone. That is how a fading edge is
// drawn: the content, then a gradient with DST_IN or DST_OUT.
//
//   Erased:  LAYER_TYPE_HARDWARE. Yellow, then its right half erased with DST_OUT. The
//            window's blue shows there.
//   Faded:   LAYER_TYPE_HARDWARE with a paint of half alpha. Red, which comes out as red
//            and blue in equal parts.
//   Added:   LAYER_TYPE_HARDWARE with a paint whose blend mode is ADD. Red, which comes
//            out as red plus the window's blue.
//   Tinted:  LAYER_TYPE_HARDWARE with a paint that has a color filter (green, SRC_IN).
//            Red, which comes out green.
//   Node:    a RenderNode with setUseCompositingLayer(true, null) and setClipToBounds(false),
//            drawn by a view, as Compose draws a graphics layer with
//            CompositingStrategy.Offscreen. Yellow drawn 40 px wider than the node on each
//            side, then its right half erased. A layer is the node's size: nothing shows
//            outside it.
//
// The test reads the colors off a screenshot.
package com.acl.layer;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.PorterDuffXfermode;
import android.graphics.RecordingCanvas;
import android.graphics.RenderNode;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.FrameLayout;

public class Main extends Activity {
    static final int BLUE = 0xFF2020C0, YELLOW = 0xFFE0C020, RED = 0xFFC02020;
    static final int GREEN = 0xFF20C020;
    static final int SIZE = 200, TOP = 40, SECOND_ROW = 260;

    static Paint erasing() {
        final Paint p = new Paint();
        p.setColor(0xFF000000);
        p.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_OUT));
        return p;
    }

    /** Yellow with its right half erased. */
    static final class Erased extends View {
        Erased(Context c) {
            super(c);
        }

        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(YELLOW);
            canvas.drawRect(getWidth() / 2f, 0, getWidth(), getHeight(), erasing());
        }
    }

    /** Red, all of it. */
    static final class Filled extends View {
        Filled(Context c) {
            super(c);
        }

        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(RED);
        }
    }

    /** Draws a RenderNode that is a compositing layer, at 40 px in from its own left. */
    static final class Node extends View {
        private final RenderNode mNode = new RenderNode("layer");
        private boolean mDrawn;

        Node(Context c) {
            super(c);
        }

        @Override protected void onDraw(Canvas canvas) {
            if (!canvas.isHardwareAccelerated()) {
                Log.i("acl", "LAYER: FAILED the view's canvas takes no RenderNode");
                return;
            }
            mNode.setPosition(40, 0, 40 + SIZE, SIZE);
            mNode.setClipToBounds(false);
            mNode.setUseCompositingLayer(true, null);
            final RecordingCanvas rc = mNode.beginRecording();
            final Paint yellow = new Paint();
            yellow.setColor(YELLOW);
            rc.drawRect(-40, 0, SIZE + 40, SIZE, yellow);
            rc.drawRect(SIZE / 2f, 0, SIZE + 40, SIZE, erasing());
            mNode.endRecording();
            canvas.drawRenderNode(mNode);
            if (!mDrawn) {
                mDrawn = true;
                Log.i("acl", "LAYER: drawn");
            }
        }
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(BLUE);

        final View erased = new Erased(this);
        erased.setLayerType(View.LAYER_TYPE_HARDWARE, null);
        root.addView(erased, at(40, TOP, SIZE));

        final View faded = new Filled(this);
        final Paint half = new Paint();
        half.setAlpha(128);
        faded.setLayerType(View.LAYER_TYPE_HARDWARE, half);
        root.addView(faded, at(280, TOP, SIZE));

        final View added = new Filled(this);
        final Paint add = new Paint();
        add.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.ADD));
        added.setLayerType(View.LAYER_TYPE_HARDWARE, add);
        root.addView(added, at(40, SECOND_ROW, SIZE));

        final View tinted = new Filled(this);
        final Paint tint = new Paint();
        tint.setColorFilter(new PorterDuffColorFilter(GREEN, PorterDuff.Mode.SRC_IN));
        tinted.setLayerType(View.LAYER_TYPE_HARDWARE, tint);
        root.addView(tinted, at(280, SECOND_ROW, SIZE));

        // 40 px wider than its node on each side, so that what the node draws outside
        // itself would land inside this view.
        root.addView(new Node(this), at(480, TOP, SIZE + 80));
        setContentView(root);
    }

    private static FrameLayout.LayoutParams at(int left, int top, int width) {
        final FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(width, SIZE);
        lp.leftMargin = left;
        lp.topMargin = top;
        return lp;
    }
}

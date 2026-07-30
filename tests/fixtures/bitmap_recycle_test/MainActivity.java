// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl diagnostic fixture; Android SDK Bitmap.recycle / Canvas.drawBitmap /
// View display-list caching docs. No GPL-3.0 sources.
//
// Hypothesis test for the podcast Discover white-out: a cached View display list (SkPicture) that
// recorded a drawBitmap keeps a REFERENCE to that bitmap; if the app recycles the bitmap (as image
// loaders like Glide/Coil do) WITHOUT the cached picture being re-recorded, replaying the picture draws
// a freed bitmap — which can error the GPU flush and turn the WHOLE frame white (toolbar included),
// exactly the podcast symptom (full content tree present, 201 content ops, yet white; force_record —
// which re-records fresh — fixes it). The plain fixtures (recycle_swap_test/rv_swap_test) use
// drawRect/drawText, NOT drawBitmap of recycled bitmaps, which is why they never reproduced it.
//
// A red "BITMAP" tile is drawn by a cached View. After 4s the bitmap is recycled WITHOUT invalidating
// the view (so its cached picture still references it) while a sibling keeps frames flowing. Correct:
// the tile stays (acl must keep the bitmap alive for the cached picture, or drop the stale draw).
// Broken (the bug): the frame goes white/garbage after the recycle.
package com.acl.bmrecycle;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

public class MainActivity extends Activity {

    // Draws a bitmap. onDraw runs ONCE (cached); the picture then holds a ref to `bmp`. Deliberately
    // does NOT null-check isRecycled — the whole point is the cached picture referencing a freed bitmap.
    static class BitmapView extends View {
        final Paint p = new Paint();
        Bitmap bmp;
        BitmapView(Context c, Bitmap b) { super(c); bmp = b; }
        @Override protected void onDraw(Canvas cv) {
            cv.drawColor(Color.rgb(30, 90, 160));               // blue bg -> tells "blank" from "bitmap"
            if (bmp != null) cv.drawBitmap(bmp, 40, 40, p);      // cached picture keeps a ref to bmp
        }
    }

    // A sibling that repaints every frame so the loop keeps presenting (its own invalidate does NOT
    // re-record the BitmapView — that stays cached, referencing the recycled bitmap).
    static class Ticker extends View {
        final Paint p = new Paint();
        int t = 0;
        Ticker(Context c) { super(c); }
        @Override protected void onDraw(Canvas cv) {
            cv.drawColor(Color.rgb(20, 120, 20));
            p.setColor(Color.WHITE); p.setTextSize(40); p.setAntiAlias(true);
            cv.drawText("frame " + (t++), 24, 60, p);
            postInvalidateDelayed(60);
        }
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final Bitmap bmp = Bitmap.createBitmap(400, 300, Bitmap.Config.ARGB_8888);
        Canvas bc = new Canvas(bmp);
        bc.drawColor(Color.rgb(220, 40, 40));                    // red bitmap
        Paint tp = new Paint(); tp.setColor(Color.WHITE); tp.setTextSize(56); tp.setAntiAlias(true);
        bc.drawText("BITMAP", 60, 170, tp);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);
        Ticker ticker = new Ticker(this);
        root.addView(ticker, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 100));
        BitmapView v = new BitmapView(this, bmp);
        root.addView(v, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);

        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            System.out.println("acl bmrecycle: recycling bitmap (NO invalidate of the cached view)");
            bmp.recycle();
        }, 4000);
    }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK View elevation / ViewOutlineProvider /
// ViewGroup Z-ordering docs. No GPL-3.0 sources.
//
// Minimum-viable repro of RenderNode elevation shadows + Z-ordering (Material). A white FrameLayout
// holds two overlapping rounded cards: a LOW card (green, elevation 8) and, overlapping it, a HIGH
// card (blue, elevation 40) — but the HIGH card is added to the layout FIRST, so only Z-ordering
// makes it draw on top. Correct render: (1) a soft dark shadow around each card (stronger under the
// higher one), and (2) the blue (high) card visible on top of the green (low) card in the overlap,
// despite green being added later. Casting a shadow requires an outline + elevation, and the framework
// brackets the children with enableZ so the native tree walk sorts by Z and draws shadows.
package com.acl.elevation;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Outline;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.widget.FrameLayout;

public class MainActivity extends Activity {
    static View card(Activity a, int color, float elevation, final int radius) {
        View v = new View(a);
        v.setBackgroundColor(color);
        v.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View vv, Outline o) {
                o.setRoundRect(0, 0, vv.getWidth(), vv.getHeight(), radius);
            }
        });
        v.setClipToOutline(true);
        v.setElevation(elevation);
        return v;
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);

        // HIGH card added FIRST (blue, elevation 40) — Z-order must still put it on top of the low one.
        View high = card(this, Color.rgb(40, 90, 220), 40f, 60);
        FrameLayout.LayoutParams hp = new FrameLayout.LayoutParams(520, 520, Gravity.CENTER);
        hp.leftMargin = 120;  // shift right so it overlaps the low card's right half
        root.addView(high, hp);

        // LOW card added SECOND (green, elevation 8) — later in draw order but lower Z -> under blue.
        View low = card(this, Color.rgb(40, 180, 60), 8f, 60);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(520, 520, Gravity.CENTER);
        lp.leftMargin = -120;  // shift left
        root.addView(low, lp);

        setContentView(root);
    }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK View / ViewOutlineProvider / setClipToOutline
// docs. No GPL-3.0 sources.
//
// Minimum-viable repro of RenderNode clipToOutline (the Material rounded-corner path). A white
// FrameLayout holds a centered opaque-BLUE child whose ViewOutlineProvider is a round-rect and which
// has setClipToOutline(true) — so the RenderNode carries a round-rect outline and the native tree
// walk must clip the child to it. Correct render: the four corners of the child region show WHITE
// (parent, clipped away) while the center is BLUE. Broken (no outline clip): a full BLUE square.
package com.acl.outline;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Outline;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.widget.FrameLayout;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);

        View card = new View(this);
        card.setBackgroundColor(Color.rgb(40, 90, 220));  // opaque blue
        final int radius = 160;
        card.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View v, Outline o) {
                o.setRoundRect(0, 0, v.getWidth(), v.getHeight(), radius);
            }
        });
        card.setClipToOutline(true);

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(720, 720, Gravity.CENTER);
        root.addView(card, lp);
        setContentView(root);
    }
}

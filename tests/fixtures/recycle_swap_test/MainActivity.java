// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl diagnostic fixture; Android SDK View/ViewGroup/Handler docs
// (setVisibility, addView/removeView, postDelayed). No GPL-3.0 sources.
//
// Minimal repro of the podcast Discover white-out (see rendernode-boundary-walk memory): a static
// "toolbar" + a content area whose "list" is hidden (setVisibility GONE) and swapped for freshly-added
// replacement views on a 2s timer — exactly the mid-session content swap (list <-> skeleton, with view
// add/remove like a RecyclerView rebind) that made the real app's whole content (INCLUDING the static
// toolbar) collapse to blank while the view tree stayed intact/visible/sized. If acl's display-list
// CACHE goes stale after the swap, this reproduces the collapse in isolation — with none of podcast's
// network/skeleton-lib/WorkManager noise — so updateDisplayListIfDirty can be step-debugged and this
// becomes a permanent regression fixture. Correct render: toolbar + colored rows ALWAYS visible,
// alternating green LIST rows and gray SKEL rows every 2s. Broken (the bug): goes white after a swap.
//
// ACL_SWAP_MS overrides the toggle interval (default 2000). Rows are custom Views (own RenderNode /
// display list, like podcast's artwork item views). Text + distinct colors make a GPU dump legible.
package com.acl.recycleswap;

import android.app.Activity;
import android.content.Context;
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

    // A custom-drawn row: its own RenderNode/display list (like podcast's item views). Starts in a
    // "loading" state and switches to "loaded" content via a delayed invalidate() (NO layout change) —
    // mimicking async artwork/image loads, the podcast's content-load-staleness trigger.
    static class Row extends View {
        final Paint p = new Paint();
        final int color;
        final String label;
        boolean loaded = false;
        Row(Context c, int color, String label) {
            super(c);
            this.color = color;
            this.label = label;
        }
        void load() { loaded = true; invalidate(); }  // async "image arrived": invalidate, no relayout
        @Override protected void onDraw(Canvas cv) {
            p.setColor(loaded ? color : Color.rgb(210, 210, 210));  // gray placeholder until loaded
            cv.drawRect(0, 0, getWidth(), getHeight(), p);
            p.setColor(loaded ? Color.WHITE : Color.rgb(160, 160, 160));
            p.setTextSize(44);
            p.setAntiAlias(true);
            cv.drawText(loaded ? label : (label + " …loading"), 24, getHeight() / 2f + 16, p);
        }
    }

    FrameLayout content;
    LinearLayout listView;   // the "real" content
    LinearLayout skeleton;   // the swapped-in replacement
    boolean showingList = true;
    int cycle = 0;
    final Handler h = new Handler(Looper.getMainLooper());

    // Build the list, and stagger an async load() on each row (like artwork loading in over ~1.2s):
    // each row starts as a gray placeholder, then invalidate()s to its real content. NO relayout.
    LinearLayout makeRows(int base, String tag, boolean asyncLoad) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        // ACL_HW_LAYER=1: promote the swapped container to a hardware layer (like podcast's
        // CoordinatorLayout/SkeletonLayout shimmer) — a prime suspect for the stale-cache collapse.
        if (System.getenv("ACL_HW_LAYER") != null) col.setLayerType(View.LAYER_TYPE_HARDWARE, null);
        for (int i = 0; i < 6; i++) {
            final Row r = new Row(this, base + i * 0x00101010, tag + " row " + i);
            col.addView(r, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 170));
            if (asyncLoad) h.postDelayed(r::load, 200 + i * 200L);  // staggered "image arrived"
            else r.loaded = true;
        }
        return col;
    }

    // Nest the content deep, like podcast's DecorView->...->CoordinatorLayout->FragmentContainerView->
    // ...->list (~12 levels of cached intermediate RenderNodes), so a stale cache at any level can bite.
    ViewGroup nest(ViewGroup leaf, int depth) {
        ViewGroup top = leaf;
        for (int i = 0; i < depth; i++) {
            FrameLayout f = new FrameLayout(this);
            f.addView(top, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            top = f;
        }
        return top;
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);

        // static "toolbar" — should render on EVERY frame, before/through/after every swap.
        Row toolbar = new Row(this, Color.rgb(30, 90, 160), "TOOLBAR (static)");
        root.addView(toolbar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 150));

        content = new FrameLayout(this);
        // Bury the content area ~10 levels deep (nested FrameLayouts) to mirror podcast's cached tree.
        root.addView(nest(content, 10), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        listView = makeRows(Color.rgb(40, 140, 60), "LIST", true);  // async-load the first list
        content.addView(listView);

        setContentView(root);

        final int period = envInt("ACL_SWAP_MS", 2000);
        h.postDelayed(new Runnable() {
            @Override public void run() {
                swap();
                h.postDelayed(this, period);
            }
        }, period);
    }

    // Mimic the podcast refresh: hide the list + add a fresh skeleton (like a rebind), then reverse.
    void swap() {
        cycle++;
        if (showingList) {
            listView.setVisibility(View.GONE);
            skeleton = makeRows(Color.rgb(150, 150, 150), "SKEL", false);
            content.addView(skeleton);
            System.out.println("acl recycleswap: cycle " + cycle + " -> SKELETON (list GONE, skel added)");
        } else {
            if (skeleton != null) { content.removeView(skeleton); skeleton = null; }
            // Rebuild the list fresh (like a RecyclerView rebind) WITH async loads, then reveal it.
            content.removeView(listView);
            listView = makeRows(Color.rgb(40, 140, 60), "LIST", true);
            content.addView(listView);
            System.out.println("acl recycleswap: cycle " + cycle + " -> LIST (rebuilt + async-load)");
        }
        showingList = !showingList;
    }

    static int envInt(String name, int dflt) {
        try { String v = System.getenv(name); return v == null ? dflt : Integer.parseInt(v.trim()); }
        catch (Throwable t) { return dflt; }
    }
}

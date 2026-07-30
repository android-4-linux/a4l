// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl diagnostic fixture; Android SDK + androidx.recyclerview public docs
// (RecyclerView/Adapter/ViewHolder/LinearLayoutManager, setVisibility, Handler.postDelayed). No GPL-3.0 sources.
//
// Faithful minimal repro of the podcast Discover white-out: a static toolbar + a real androidx
// RecyclerView (ViewHolder RECYCLING — the same Row View/RenderNode is reused across binds via
// RecyclerView's detach/attach, unlike the plain add/remove fixture recycle_swap_test which renders
// fine). On an ACL_SWAP_MS timer the list is hidden (GONE) and swapped for a skeleton, then the adapter
// is cleared + repopulated (rebinding the recycled rows) and revealed — exactly podcast's refresh. Each
// row starts as a gray placeholder and load()s to real content via a delayed invalidate() (no relayout),
// mimicking async artwork. If acl's display-list cache goes stale across the recycle+rebind, this
// reproduces the collapse (whole content, incl. the static toolbar, goes white) in isolation.
package com.acl.rvswap;

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

import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {

    // A custom-drawn row reused by the RecyclerView across binds. Starts as a gray placeholder, then
    // load() switches to real content via invalidate() (no relayout) — the async-artwork trigger.
    static class Row extends View {
        final Paint p = new Paint();
        int color;
        String label = "";
        boolean loaded = false;
        Row(Context c) { super(c); }
        void bind(int color, String label) { this.color = color; this.label = label; loaded = false; invalidate(); }
        void load() { loaded = true; invalidate(); }
        @Override protected void onDraw(Canvas cv) {
            p.setColor(loaded ? color : Color.rgb(210, 210, 210));
            cv.drawRect(0, 0, getWidth(), getHeight(), p);
            p.setColor(loaded ? Color.WHITE : Color.rgb(160, 160, 160));
            p.setTextSize(44);
            p.setAntiAlias(true);
            cv.drawText(loaded ? label : (label + " …loading"), 24, getHeight() / 2f + 16, p);
        }
    }

    static class Adapter extends RecyclerView.Adapter<Adapter.VH> {
        static class VH extends RecyclerView.ViewHolder {
            final Row row;
            VH(Row r) { super(r); row = r; }
        }
        final List<String> items = new ArrayList<>();
        final Handler h;
        Adapter(Handler h) { this.h = h; }
        @Override public VH onCreateViewHolder(ViewGroup parent, int viewType) {
            Row r = new Row(parent.getContext());
            r.setLayoutParams(new RecyclerView.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 170));
            return new VH(r);
        }
        @Override public void onBindViewHolder(VH vh, int pos) {
            vh.row.bind(Color.rgb(40, 140, 60) + pos * 0x00080808, items.get(pos));
            h.postDelayed(vh.row::load, 250 + (pos % 6) * 150L);  // staggered async "artwork arrived"
        }
        @Override public int getItemCount() { return items.size(); }
    }

    FrameLayout content;
    RecyclerView rv;
    SwipeRefreshLayout srl;   // podcast's list lives inside a (Multi)SwipeRefreshLayout
    Adapter adapter;
    LinearLayout skeleton;
    boolean showingList = true;
    int cycle = 0;
    final Handler h = new Handler(Looper.getMainLooper());

    void populate() {
        adapter.items.clear();
        for (int i = 0; i < 15; i++) adapter.items.add("Podcast item " + i);
        adapter.notifyDataSetChanged();
    }

    LinearLayout makeSkeleton() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < 6; i++) {
            Row r = new Row(this);
            r.bind(Color.rgb(150, 150, 150), "SKEL " + i);
            r.loaded = true;
            col.addView(r, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 170));
        }
        return col;
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);

        Row toolbar = new Row(this);
        toolbar.bind(Color.rgb(30, 90, 160), "TOOLBAR (static)");
        toolbar.loaded = true;
        root.addView(toolbar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 150));

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        rv = new RecyclerView(this);
        rv.setLayoutManager(new LinearLayoutManager(this));
        adapter = new Adapter(h);
        rv.setAdapter(adapter);
        srl = new SwipeRefreshLayout(this);
        srl.addView(rv, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        content.addView(srl, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        populate();

        setContentView(root);

        final int period = envInt("ACL_SWAP_MS", 2000);
        h.postDelayed(new Runnable() {
            @Override public void run() { swap(); h.postDelayed(this, period); }
        }, period);
    }

    void swap() {
        cycle++;
        if (showingList) {
            srl.setRefreshing(true);          // show the refresh spinner (podcast's pull-to-refresh)
            srl.setVisibility(View.GONE);
            skeleton = makeSkeleton();
            content.addView(skeleton);
            System.out.println("acl rvswap: cycle " + cycle + " -> SKELETON (srl GONE, refreshing)");
        } else {
            if (skeleton != null) { content.removeView(skeleton); skeleton = null; }
            populate();                       // clear + repopulate: rebinds the recycled rows
            srl.setRefreshing(false);
            srl.setVisibility(View.VISIBLE);
            System.out.println("acl rvswap: cycle " + cycle + " -> LIST (repopulated, srl VISIBLE)");
        }
        showingList = !showingList;
    }

    static int envInt(String name, int dflt) {
        try { String v = System.getenv(name); return v == null ? dflt : Integer.parseInt(v.trim()); }
        catch (Throwable t) { return dflt; }
    }
}

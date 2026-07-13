// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK android.view.View / android.graphics
// (public API). No GPL-3.0 sources.
//
// Software readback ORACLE for the real View traversal (a pixel check, NOT the product
// path): construct DrawView with the runtime's minimal Context, measure/layout, draw into a
// Bitmap Canvas, and report a center pixel. The product path is acl-driven GPU
// (ArtVm::graphics_view_demo → present_content_loop_gpu on the Ganesh Vulkan Canvas).
// Framework setup (Looper, no-Binder ServiceManager, Context) is provided by acl's runtime
// bootstrap org.acl.internal.HostBootstrap — the app itself stays pure.
package com.acl.hello;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.view.View;
import org.acl.internal.HostBootstrap;

public class Main {
    public static void main(String[] args) throws Exception {
        HostBootstrap.init();
        final int W = 240, H = 160;
        View v = new DrawView(HostBootstrap.newContext());
        v.measure(View.MeasureSpec.makeMeasureSpec(W, View.MeasureSpec.EXACTLY),
                  View.MeasureSpec.makeMeasureSpec(H, View.MeasureSpec.EXACTLY));
        v.layout(0, 0, W, H);

        Bitmap bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);
        v.draw(canvas);
        int center = bmp.getPixel(W / 2, H / 2);
        int corner = bmp.getPixel(2, 2);
        System.out.println(String.format(
                "VIEW PROBE: %dx%d center=0x%08x corner=0x%08x", W, H, center, corner));
        System.out.println("VIEW PROBE: real android.view.View.onDraw ran on host ART");
    }
}

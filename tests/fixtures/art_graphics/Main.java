// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK android.graphics (Bitmap/Canvas/Paint,
// public API). No GPL-3.0 sources.
//
// Probe for the ART graphics native floor (P2.4/V3, ADR-0008): draw with the REAL
// android.graphics stack on host ART — create an ARGB_8888 Bitmap, a Canvas over it,
// drawColor + drawRect through a Paint, then read pixels back. This works only because
// acl registered the host graphics natives (register_android_graphics_classes +
// SkGraphics::Init, gated by ACL_ART_GRAPHICS). Reached via reflection so the fixture
// needs no compile-time framework dependency (compiled with a bare javac, no android.jar).
package com.acl.hello;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

public class Main {
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        final int W = 16, H = 10;
        final int BG = 0xFF204060, FG = 0xFFCC8844;
        final int L = 3, T = 2, R = 11, B = 7;

        Class<?> bitmapC = Class.forName("android.graphics.Bitmap");
        Class<?> configC = Class.forName("android.graphics.Bitmap$Config");
        Object argb8888 = Enum.valueOf((Class<Enum>) configC.asSubclass(Enum.class), "ARGB_8888");

        // Bitmap.createBitmap(w, h, ARGB_8888) — the native nativeCreate allocates a
        // real host-Skia SkBitmap.
        Method createBitmap = bitmapC.getMethod("createBitmap", int.class, int.class, configC);
        Object bitmap = createBitmap.invoke(null, W, H, argb8888);

        // new Canvas(bitmap) — binds the native SkCanvas to the bitmap's pixels.
        Class<?> canvasC = Class.forName("android.graphics.Canvas");
        Constructor<?> canvasCtor = canvasC.getConstructor(bitmapC);
        Object canvas = canvasCtor.newInstance(bitmap);

        // canvas.drawColor(BG) then drawRect(L,T,R,B, paint) with a Paint(FG).
        canvasC.getMethod("drawColor", int.class).invoke(canvas, BG);
        Class<?> paintC = Class.forName("android.graphics.Paint");
        Object paint = paintC.getConstructor().newInstance();
        paintC.getMethod("setColor", int.class).invoke(paint, FG);
        canvasC.getMethod("drawRect", float.class, float.class, float.class, float.class, paintC)
                .invoke(canvas, (float) L, (float) T, (float) R, (float) B, paint);

        // Read pixels back through the real native (Bitmap.getPixel -> nativeGetPixel).
        Method getPixel = bitmapC.getMethod("getPixel", int.class, int.class);
        int bg = (Integer) getPixel.invoke(bitmap, 0, 0);       // outside the rect
        int fg = (Integer) getPixel.invoke(bitmap, 5, 4);       // inside the rect
        int edge = (Integer) getPixel.invoke(bitmap, R - 1, B - 1);  // rect's last pixel
        System.out.println(String.format(
                "GRAPHICS PROBE: bg(0,0)=0x%08x fg(5,4)=0x%08x edge(10,6)=0x%08x", bg, fg, edge));
        System.out.println("GRAPHICS PROBE: bitmap " + W + "x" + H + " drawn on real host Skia");
    }
}

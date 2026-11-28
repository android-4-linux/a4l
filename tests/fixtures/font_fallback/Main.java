// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (Paint.measureText,
// Paint.getTextWidths, Paint.hasGlyph, Canvas.drawText). No GPL-3.0 sources.
//
// Text with a character the Paint's typeface has no glyph for: the katakana middle dot
// (U+30FB) an app puts between two words, in a UI font that is Latin only. Android draws it
// from another of the system's fonts. The fixture measures and draws such text into a
// Bitmap and logs what it finds; the test reads the log.
//
// It also measures a String with a character past ASCII, which a4l measured short: the
// String's indexes were applied to its UTF-8.
package com.acl.fontfallback;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.util.Log;
import android.view.View;

public class Main extends Activity {
    static final String DOT = "・";
    static final int SIZE = 100;

    static void log(String s) {
        Log.i("acl", "FONT: " + s);
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(new View(this));
        try {
            check();
        } catch (Throwable t) {
            log("FAILED " + t);
        }
        log("done");
        System.exit(0);
    }

    private static boolean near(float a, float b) {
        return Math.abs(a - b) < 0.5f;
    }

    /** The box of the dark pixels of a white bitmap, as {count, left, top, right, bottom}. */
    private static int[] ink(Bitmap bm) {
        int n = 0, l = bm.getWidth(), t = bm.getHeight(), r = -1, bt = -1;
        for (int y = 0; y < bm.getHeight(); y++) {
            for (int x = 0; x < bm.getWidth(); x++) {
                if ((bm.getPixel(x, y) & 0xFF) < 128) {
                    n++;
                    l = Math.min(l, x);
                    t = Math.min(t, y);
                    r = Math.max(r, x);
                    bt = Math.max(bt, y);
                }
            }
        }
        return new int[]{n, l, t, r, bt};
    }

    private void check() {
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setTextSize(SIZE);
        p.setColor(0xFF000000);

        // A String and a char[] measure the same, with a character past ASCII in them.
        final float a = p.measureText("A");
        final float word = p.measureText("öA");
        final float chars = p.measureText(new char[]{'ö', 'A'}, 0, 2);
        final float[] each = new float[2];
        p.getTextWidths("öA", each);
        log("past ASCII: string and chars agree=" + (word > a && near(word, chars))
                + " widths add up=" + near(each[0] + each[1], word)
                + " A after it=" + near(each[1], a));

        // The dot is some font's, and a code point that is no character is no font's.
        log("hasGlyph dot=" + p.hasGlyph(DOT) + " A=" + p.hasGlyph("A")
                + " U+10FFFE=" + p.hasGlyph("􏿾"));

        // Its advance is the other font's, and a run across the two fonts adds up.
        final float dot = p.measureText(DOT);
        final float b = p.measureText("B");
        final float[] three = new float[3];
        p.getTextWidths("A" + DOT + "B", three);
        log("dot advance=" + Math.round(dot) + " run adds up="
                + near(p.measureText("A" + DOT + "B"), a + dot + b)
                + " widths=" + (near(three[0], a) && near(three[1], dot) && near(three[2], b)));

        // Drawn, it is a dot: a small blob, and not the box of a missing glyph, which is
        // most of the height of a capital.
        final Bitmap one = Bitmap.createBitmap(200, 160, Bitmap.Config.ARGB_8888);
        final Canvas c = new Canvas(one);
        c.drawColor(0xFFFFFFFF);
        c.drawText(DOT, 40, 120, p);
        final int[] box = ink(one);
        final int w = box[3] - box[1] + 1, h = box[4] - box[2] + 1;
        log("dot drawn: ink=" + (box[0] > 20) + " a dot=" + (box[0] > 20 && w <= 40 && h <= 40)
                + " (" + (box[0] > 0 ? w + "x" + h : "nothing") + ")");

        // In a run, what follows the dot starts where the dot's advance ends.
        final Bitmap run = Bitmap.createBitmap(420, 160, Bitmap.Config.ARGB_8888);
        final Canvas rc = new Canvas(run);
        rc.drawColor(0xFFFFFFFF);
        rc.drawText("A" + DOT + "B", 20, 120, p);
        final int end = ink(run)[3];
        final float want = 20 + a + dot + b;
        log("run drawn: B ends at the run's advance=" + (end > want - 20 && end <= want + 2));

        // An emoji, which is a character outside the BMP and a color glyph where a font
        // has one. Logged and not checked: it is the host's font that decides.
        final String emoji = "\uD83D\uDE00";
        final Bitmap face = Bitmap.createBitmap(200, 160, Bitmap.Config.ARGB_8888);
        final Canvas fc = new Canvas(face);
        fc.drawColor(0xFFFFFFFF);
        fc.drawText(emoji, 40, 120, p);
        int colored = 0;
        for (int y = 0; y < face.getHeight(); y++) {
            for (int x = 0; x < face.getWidth(); x++) {
                final int px = face.getPixel(x, y);
                final int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, bl = px & 0xFF;
                if (Math.max(r, Math.max(g, bl)) - Math.min(r, Math.min(g, bl)) > 40) colored++;
            }
        }
        final float[] pair = new float[2];
        p.getTextWidths(emoji, pair);
        log("emoji (not checked): hasGlyph=" + p.hasGlyph(emoji) + " advance="
                + Math.round(p.measureText(emoji)) + " on its first unit="
                + (pair[1] == 0f && near(pair[0], p.measureText(emoji)))
                + " colored pixels=" + colored);
    }
}

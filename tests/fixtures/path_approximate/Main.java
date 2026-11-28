// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (Path.approximate,
// PathInterpolator). No GPL-3.0 sources.
//
// A path as line segments, which is what Path.approximate gives and what a PathInterpolator
// and a path animation are built on. Android flattens each curve only as far as the error
// asked for needs, at points on the curve itself, and goes through every contour: a move in
// the path shows as two points at one fraction of its length.
//
// The fixture logs what approximate gives for paths whose answer is known exactly, checks an
// arc's points against its circle, and makes an interpolator from each cubic with its control
// points' x on a tenth from 0 to 1. All of those rise in x, and Android takes all of them;
// the ones with a control point at 0 or 1 are level at an end, where a point taken a hair off
// the curve reads as the path turning back.
package com.acl.pathapprox;

import android.app.Activity;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.animation.PathInterpolator;

public class Main extends Activity {
    static void log(String s) {
        Log.i("acl", "PATH: " + s);
    }

    /** The points as "fraction:x,y", a space between. */
    private static String points(float[] a) {
        final StringBuilder sb = new StringBuilder();
        for (int i = 0; i + 2 < a.length; i += 3) {
            if (i > 0) sb.append(' ');
            sb.append(a[i]).append(':').append(a[i + 1]).append(',').append(a[i + 2]);
        }
        return a.length % 3 == 0 ? sb.toString() : sb + " (length " + a.length + ")";
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(new View(this));
        try {
            exact();
            log("arc " + arc());
            log("interpolators " + interpolators());
        } catch (Throwable t) {
            log("FAILED " + t);
        }
        log("done");
        System.exit(0);
    }

    private static void exact() {
        final Path line = new Path();
        line.moveTo(0, 0);
        line.lineTo(10, 0);
        log("line " + points(line.approximate(0.5f)));

        // Two contours of the same length: the move is two points at half the length.
        final Path two = new Path();
        two.moveTo(0, 0);
        two.lineTo(10, 0);
        two.moveTo(20, 5);
        two.lineTo(20, 15);
        log("contours " + points(two.approximate(0.5f)));

        // A closed contour ends where it began.
        final Path box = new Path();
        box.addRect(0, 0, 10, 10, Path.Direction.CW);
        log("closed " + points(box.approximate(0.5f)));

        // A path that is one point is that point from start to end.
        final Path point = new Path();
        point.moveTo(3, 4);
        log("point " + points(point.approximate(0.5f)));
        log("empty " + points(new Path().approximate(0.5f)));
    }

    /** A quarter circle of radius 100 about (100, 100), which Skia keeps as a conic. */
    private static String arc() {
        final Path p = new Path();
        p.arcTo(new RectF(0, 0, 200, 200), 180, 90, true);
        final float[] a = p.approximate(0.5f);
        final int n = a.length / 3;
        if (a.length % 3 != 0 || n < 3) return "points=" + n;
        double off = 0;
        double sag = 0;
        boolean rising = a[0] == 0f && a[a.length - 3] == 1f;
        for (int i = 0; i < n; i++) {
            final double r = Math.hypot(a[i * 3 + 1] - 100, a[i * 3 + 2] - 100);
            off = Math.max(off, Math.abs(r - 100));
            if (i == 0) continue;
            rising &= a[i * 3] > a[i * 3 - 3];
            // How far the chord between two points falls inside the circle.
            final double mx = (a[i * 3 + 1] + a[i * 3 - 2]) / 2.0 - 100;
            final double my = (a[i * 3 + 2] + a[i * 3 - 1]) / 2.0 - 100;
            sag = Math.max(sag, 100 - Math.hypot(mx, my));
        }
        final boolean ends = a[1] == 0f && a[2] == 100f
                && Math.abs(a[a.length - 2] - 100) < 1e-3 && Math.abs(a[a.length - 1]) < 1e-3;
        return "on circle=" + (off < 0.05) + " chords within=" + (sag < 1.0)
                + " fractions rise=" + rising + " ends=" + ends + " few=" + (n <= 64);
    }

    /** x and y of the cubic (0,0) (x1,y1) (x2,y2) (1,1) at t. */
    private static double at(double t, double c1, double c2) {
        final double u = 1 - t;
        return 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t;
    }

    private static String interpolators() {
        int made = 0;
        int all = 0;
        double worst = 0;
        String first = null;
        for (int i = 0; i <= 10; i++) {
            for (int j = 0; j <= 10; j++) {
                final float x1 = i / 10f;
                final float x2 = j / 10f;
                all++;
                final PathInterpolator p;
                try {
                    p = new PathInterpolator(x1, 0f, x2, 1f);
                } catch (IllegalArgumentException e) {
                    if (first == null) first = "(" + x1 + ", 0, " + x2 + ", 1): " + e.getMessage();
                    continue;
                }
                made++;
                if (p.getInterpolation(0f) != 0f || p.getInterpolation(1f) != 1f) {
                    if (first == null) first = "(" + x1 + ", 0, " + x2 + ", 1): ends";
                    continue;
                }
                // Against the curve itself: t for x by halving, then y at that t.
                for (int k = 1; k < 20; k++) {
                    final double x = k / 20.0;
                    double lo = 0;
                    double hi = 1;
                    for (int s = 0; s < 50; s++) {
                        final double mid = (lo + hi) / 2;
                        if (at(mid, x1, x2) < x) lo = mid; else hi = mid;
                    }
                    final double want = at((lo + hi) / 2, 0, 1);
                    worst = Math.max(worst, Math.abs(p.getInterpolation((float) x) - want));
                }
            }
        }
        return "made=" + made + "/" + all + " close=" + (worst < 0.02)
                + (first != null ? " first failure " + first : "");
    }
}

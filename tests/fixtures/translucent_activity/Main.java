// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (Activity lifecycle,
// R.attr.windowIsTranslucent, WindowManager.LayoutParams.FLAG_DIM_BEHIND). No GPL-3.0 sources.
//
// An Activity with a translucent theme over another: a welcome card over a map. On Android
// the Activity underneath stays on screen. It is paused, not stopped, its SurfaceView keeps
// its surface, and it is resumed when the one over it finishes.
//
// Main, the launcher, shows a plain view and starts Map, as the GL fixture does: an app whose
// first Activity has the GL view is given the whole window as its GL surface instead. Map
// lays the GL fixture's MapView under a red banner and starts Welcome, whose theme is
// Theme.Translucent.NoTitleBar, as soon as it is resumed, as an app that asks its user to
// sign in does. Welcome draws a yellow card in the middle and a half-black strip down the
// right edge, and leaves the rest of its window clear. Cover is an opaque Activity started
// over Welcome.
//
// The test drives it through a control file named by A4L_TEST_CONTROL, which holds a
// sequence number and a command: close (finish Welcome), open (start it again, with the
// window's dim on), cover (start Cover over Welcome), uncover (finish Cover).
package com.acl.translucent;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.io.BufferedReader;
import java.io.FileReader;

public class Main extends Activity {
    static final int BANNER_HEIGHT = 96;
    static Map sMap;
    static Welcome sWelcome;
    static Cover sCover;
    static boolean sDim;
    static int sSeen;

    static void log(String s) {
        Log.i("acl", "TRANSLUCENT: " + s);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        final View plain = new View(this);
        plain.setBackgroundColor(0xFF404040);
        setContentView(plain);
        plain.postDelayed(new Runnable() {
            @Override public void run() {
                startActivity(new Intent(Main.this, Map.class));
            }
        }, 300);
    }

    static void poll(String control) {
        final String line;
        try (BufferedReader r = new BufferedReader(new FileReader(control))) {
            line = r.readLine();
        } catch (java.io.IOException e) {
            return;  // not written yet
        }
        final String[] parts = line == null ? new String[0] : line.trim().split(" ");
        if (parts.length != 2) return;
        final int n;
        try {
            n = Integer.parseInt(parts[0]);
        } catch (NumberFormatException e) {
            return;  // half written
        }
        if (n <= sSeen) return;
        sSeen = n;
        log("command " + parts[1]);
        if ("close".equals(parts[1]) && sWelcome != null) {
            sWelcome.finish();
        } else if ("open".equals(parts[1])) {
            sDim = true;
            sMap.startActivity(new Intent(sMap, Welcome.class));
        } else if ("cover".equals(parts[1]) && sWelcome != null) {
            sWelcome.startActivity(new Intent(sWelcome, Cover.class));
        } else if ("uncover".equals(parts[1]) && sCover != null) {
            sCover.finish();
        }
    }

    public static class Map extends Activity {
        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            sMap = this;
            final FrameLayout root = new FrameLayout(this);
            root.addView(new com.acl.glplane.Main.MapView(this), new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT));
            final View banner = new View(this);
            banner.setBackgroundColor(0xFFC02020);
            root.addView(banner, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, BANNER_HEIGHT, Gravity.TOP));
            setContentView(root);
            log("map onCreate");
            final Handler h = new Handler(Looper.getMainLooper());
            // Once this Activity is resumed.
            h.post(new Runnable() {
                @Override public void run() {
                    startActivity(new Intent(Map.this, Welcome.class));
                }
            });
            final String control = System.getenv("A4L_TEST_CONTROL");
            if (control == null) return;
            h.postDelayed(new Runnable() {
                @Override public void run() {
                    poll(control);
                    h.postDelayed(this, 50);
                }
            }, 50);
        }

        @Override protected void onStart() { super.onStart(); log("map onStart"); }
        @Override protected void onRestart() { super.onRestart(); log("map onRestart"); }
        @Override protected void onResume() { super.onResume(); log("map onResume"); }
        @Override protected void onPause() { super.onPause(); log("map onPause"); }
        @Override protected void onStop() { super.onStop(); log("map onStop"); }
    }

    public static class Welcome extends Activity {
        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            sWelcome = this;
            if (sDim) {
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
                getWindow().setDimAmount(0.5f);
            }
            final FrameLayout root = new FrameLayout(this);
            final View card = new View(this);
            card.setBackgroundColor(0xFFE0C020);
            root.addView(card, new FrameLayout.LayoutParams(200, 100, Gravity.CENTER));
            final View strip = new View(this);
            strip.setBackgroundColor(0x80000000);
            root.addView(strip, new FrameLayout.LayoutParams(
                    200, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.RIGHT));
            setContentView(root);
            log("welcome onCreate");
        }

        @Override protected void onRestart() { super.onRestart(); log("welcome onRestart"); }
        @Override protected void onResume() { super.onResume(); log("welcome onResume"); }
        @Override protected void onPause() { super.onPause(); log("welcome onPause"); }
        @Override protected void onStop() { super.onStop(); log("welcome onStop"); }

        @Override
        protected void onDestroy() {
            super.onDestroy();
            if (sWelcome == this) sWelcome = null;
            log("welcome onDestroy");
        }
    }

    public static class Cover extends Activity {
        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            sCover = this;
            final View v = new View(this);
            v.setBackgroundColor(0xFFC020C0);
            setContentView(v);
        }

        @Override protected void onResume() { super.onResume(); log("cover onResume"); }
    }
}

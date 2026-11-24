// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK public API (android.view.ViewTreeObserver,
// Activity.startActivity). No GPL-3.0 sources.
//
// A view's ViewTreeObserver listeners, in the shape a splash screen has: the launcher Activity
// starts the real one from its onCreate and finishes, and the real one's view registers an
// OnGlobalLayoutListener before it is attached to a window. The view then waits for that
// callback, as a logo waits for its size before it starts to animate. Main is both Activities:
// the second is started with the extra "second".
//
// Logs "TREE:" lines the test reads, ending "TREE: PASS" (exit 0). A callback that does not come
// is caught by a 20 s limit (exit 3).
package com.acl.tree;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Canvas;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;

public class Main extends Activity {
    static final String TAG = "acl";
    final Handler main = new Handler(Looper.getMainLooper());

    static void log(String s) {
        Log.i(TAG, "TREE: " + s);
    }

    static void fail(String why) {
        log("FAIL " + why);
        System.exit(1);
    }

    /** Counts its draws, so the test can tell a cancelled frame from a drawn one. */
    static class Probe extends View {
        int draws;

        Probe(Activity a) {
            super(a);
            setBackgroundColor(0xff2060a0);
        }

        @Override protected void onDraw(Canvas c) {
            draws++;
        }
    }

    int layouts, preDraws, drawCalls, drawsAtFirstPass = -1;
    int layoutsAtRequest = -1;  // set when the test asks for a layout
    Probe probe;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Intent in = getIntent();
        if (in == null || !in.getBooleanExtra("second", false)) {
            // The launcher: hand over to the real Activity before any frame, as a splash does.
            startActivity(new Intent(this, Main.class).putExtra("second", true));
            finish();
            return;
        }
        main.postDelayed(new Runnable() {
            @Override public void run() {
                log("FAIL timed out: layouts=" + layouts + " preDraws=" + preDraws
                        + " drawCalls=" + drawCalls);
                System.exit(3);
            }
        }, 20000);

        probe = new Probe(this);
        // Registered before the view is in a window: kept by the view until it is attached.
        final ViewTreeObserver early = probe.getViewTreeObserver();
        early.addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override public void onGlobalLayout() {
                layouts++;
                if (layouts == 1) {
                    log("global layout " + (probe.getWidth() > 0 && probe.getHeight() > 0
                            ? "sized" : "0x0") + " draws=" + probe.draws);
                } else if (layoutsAtRequest >= 0) {
                    layoutsAtRequest = -1;
                    log("global layout again after requestLayout");
                    finishUp();
                }
            }
        });
        setContentView(probe);

        // Registered on the window's own observer once the view is in the tree.
        final ViewTreeObserver tree = getWindow().getDecorView().getViewTreeObserver();
        tree.addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
            @Override public boolean onPreDraw() {
                preDraws++;
                if (preDraws == 1) {
                    // Cancel the first frame: nothing may be drawn until a later pass.
                    drawsAtFirstPass = probe.draws;
                    probe.invalidate();
                    return false;
                }
                if (preDraws == 2) {
                    log("pre-draw cancelled one frame: draws then=" + drawsAtFirstPass);
                    // A layout a view asks for later is reported too.
                    main.postDelayed(new Runnable() {
                        @Override public void run() {
                            layoutsAtRequest = layouts;
                            probe.requestLayout();
                        }
                    }, 200);
                }
                return true;
            }
        });
        tree.addOnDrawListener(new ViewTreeObserver.OnDrawListener() {
            @Override public void onDraw() {
                drawCalls++;
            }
        });
    }

    void finishUp() {
        main.postDelayed(new Runnable() {
            @Override public void run() {
                log("draw listener called=" + (drawCalls > 0) + " view drawn=" + (probe.draws > 0));
                if (drawsAtFirstPass != 0) fail("the view was drawn before its first pre-draw");
                if (drawCalls == 0 || probe.draws == 0) fail("no draw after pre-draw allowed it");
                log("PASS");
                System.exit(0);
            }
        }, 200);
    }
}

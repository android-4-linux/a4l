// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK public API (android.view.ViewTreeObserver,
// Activity.startActivity). No GPL-3.0 sources.
//
// A view's ViewTreeObserver listeners, in the shape a splash screen has: the launcher Activity
// starts the real one from its onCreate and finishes, and the real one's view registers an
// OnGlobalLayoutListener before it is attached to a window. The view then waits for that
// callback, as a logo waits for its size before it starts to animate. Main is the launcher and
// Second, a subclass, the Activity it starts. The manifest gives each its own theme (dark for
// the launcher, light for Second), and Second must come up in its own.
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

    /** The Activity the launcher starts; declared with its own theme. */
    public static class Second extends Main {}

    static boolean sLauncherLight;

    boolean isLightTheme() {
        final android.util.TypedValue v = new android.util.TypedValue();
        return getTheme().resolveAttribute(android.R.attr.isLightTheme, v, true) && v.data != 0;
    }

    int layouts, preDraws, drawCalls, drawsAtFirstPass = -1;
    int layoutsAtRequest = -1;  // set when the test asks for a layout
    Probe probe;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (!(this instanceof Second)) {
            // The launcher: hand over to the real Activity before any frame, as a splash does.
            sLauncherLight = isLightTheme();
            startActivity(new Intent(this, Second.class));
            finish();
            return;
        }
        log("theme launcher light=" + sLauncherLight + " second light=" + isLightTheme());
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
                // Last, the task: Second is declared noHistory, so starting Home over it
                // finishes it.
                startActivity(new Intent(Main.this, Home.class));
            }
        }, 200);
    }

    static boolean sSecondDestroyed;

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (this instanceof Second) sSecondDestroyed = true;
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        log("FAIL came back to an Activity the new task cleared");
        System.exit(1);
    }

    /**
     * Started twice. First over Second, which is noHistory and so is destroyed. Then again with
     * NEW_TASK | CLEAR_TASK, which destroys the first Home. When the second Home finishes the
     * task is empty and the app ends instead of going back to anything.
     */
    public static class Home extends Activity {
        static int sHomes, sHomesDestroyed;
        int nth;

        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            nth = ++sHomes;
            setContentView(new View(this));
        }

        @Override
        protected void onDestroy() {
            super.onDestroy();
            sHomesDestroyed++;
        }

        @Override
        protected void onRestart() {
            super.onRestart();
            log("FAIL came back to a Home the new task cleared");
            System.exit(1);
        }

        @Override
        protected void onResume() {
            super.onResume();
            final Handler h = new Handler(Looper.getMainLooper());
            h.postDelayed(new Runnable() {
                @Override public void run() {
                    if (nth == 1) {
                        log("no history second destroyed=" + sSecondDestroyed);
                        if (!sSecondDestroyed) System.exit(1);
                        startActivity(new Intent(Home.this, Home.class).addFlags(
                                Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
                        return;
                    }
                    log("clear task first home destroyed=" + (sHomesDestroyed == 1));
                    if (sHomesDestroyed != 1) System.exit(1);
                    h.postDelayed(new Runnable() {
                        @Override public void run() {
                            log("FAIL the app kept running after its last Activity finished");
                            System.exit(4);
                        }
                    }, 3000);
                    log("PASS");
                    finish();
                }
            }, 100);
        }
    }
}

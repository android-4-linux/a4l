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
            // A RenderNode recorded before any window is up, as an app that draws into its
            // own nodes at start does. The frame the test reads must still be the window's.
            final android.graphics.RenderNode first = new android.graphics.RenderNode("first");
            first.setPosition(0, 0, 4, 4);
            first.beginRecording().drawColor(0xff00ff00);
            first.endRecording();
            log("render node recorded before the window=" + first.hasDisplayList());
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
     * Blue, with one red child that fills it and is clipped to the top half in drawChild, the
     * way a sliding panel reveals part of a child. The test reads the frame: red above the
     * middle, blue below.
     */
    public static class ClipGroup extends android.view.ViewGroup {
        final View child;
        /**
         * 100 wide and 200 tall, laid out at the top left and moved 300 down with
         * translationY. Tall, so the test's two taps land inside the old and the new place
         * whether or not the window has an action bar above the content.
         */
        final View moved;
        /** Run when a touch reaches `moved` where it is drawn. */
        Runnable onMovedTouched;

        ClipGroup(android.content.Context c) {
            super(c);
            setBackgroundColor(0xff0000ff);
            child = new View(c);
            child.setBackgroundColor(0xffff0000);
            addView(child);
            moved = new View(c);
            moved.setBackgroundColor(0xff00ff00);
            moved.setTranslationY(300);
            moved.setOnTouchListener(new View.OnTouchListener() {
                @Override public boolean onTouch(View v, android.view.MotionEvent e) {
                    if (e.getActionMasked() != android.view.MotionEvent.ACTION_DOWN) return true;
                    if (e.getRawY() < 250) {
                        // The test also taps (50,150), where the view was laid out and is not.
                        log("FAIL a touch at y=" + e.getRawY()
                                + " reached a view that translationY moved to y=300");
                        System.exit(1);
                    }
                    log("touch reached the moved view");
                    if (onMovedTouched != null) onMovedTouched.run();
                    return true;
                }
            });
            addView(moved);
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            child.layout(0, 0, r - l, b - t);
            moved.layout(0, 0, 100, 200);
        }

        @Override
        protected boolean drawChild(Canvas canvas, View v, long time) {
            if (v != child) return super.drawChild(canvas, v, time);
            final int save = canvas.save();
            canvas.clipRect(0, 0, getWidth(), getHeight() / 2);
            final boolean more = super.drawChild(canvas, v, time);
            canvas.restoreToCount(save);
            return more;
        }
    }

    /**
     * Started twice. First over Second, which is noHistory and so is destroyed. Then again with
     * NEW_TASK | CLEAR_TASK, which destroys the first Home. When the second Home finishes the
     * task is empty and the app ends instead of going back to anything.
     */
    public static class Home extends Activity {
        static int sHomes, sHomesDestroyed;
        int nth;
        ClipGroup group;

        /** Launch callbacks in the order they came: c, s, C (post create), r, R (post resume). */
        final StringBuilder launch = new StringBuilder();

        @Override
        protected void onStart() {
            super.onStart();
            launch.append('s');
        }

        @Override
        protected void onPostCreate(Bundle b) {
            super.onPostCreate(b);
            launch.append('C');
        }

        @Override
        protected void onPostResume() {
            super.onPostResume();
            launch.append('R');
        }

        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            launch.append('c');
            nth = ++sHomes;
            group = new ClipGroup(this);
            setContentView(group);
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
            launch.append('r');
            final Handler h = new Handler(Looper.getMainLooper());
            h.postDelayed(new Runnable() {
                @Override public void run() {
                    if (nth == 1) {
                        log("started activity launch order " + launch);
                        if (!"csCrR".contentEquals(launch)) System.exit(1);
                        log("no history second destroyed=" + sSecondDestroyed);
                        if (!sSecondDestroyed) System.exit(1);
                        startActivity(new Intent(Home.this, Home.class).addFlags(
                                Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
                        return;
                    }
                    log("clear task first home destroyed=" + (sHomesDestroyed == 1));
                    if (sHomesDestroyed != 1) System.exit(1);
                    // A view Animation runs on the frame's drawing time. It has to end.
                    final Runnable noEnd = new Runnable() {
                        @Override public void run() {
                            log("FAIL a view animation did not end");
                            System.exit(6);
                        }
                    };
                    h.postDelayed(noEnd, 5000);
                    final android.view.animation.AlphaAnimation fade =
                            new android.view.animation.AlphaAnimation(0f, 1f);
                    fade.setDuration(200);
                    fade.setAnimationListener(
                            new android.view.animation.Animation.AnimationListener() {
                        @Override public void onAnimationStart(
                                android.view.animation.Animation a) {}
                        @Override public void onAnimationRepeat(
                                android.view.animation.Animation a) {}
                        @Override public void onAnimationEnd(android.view.animation.Animation a) {
                            h.removeCallbacks(noEnd);
                            log("view animation ended");
                            addedWindow(h);
                        }
                    });
                    group.moved.startAnimation(fade);
                }
            }, 100);
        }

        /**
         * A window added with WindowManager.addView: its tree observer's pre-draw listener is
         * called, as a viewer that starts its opening animation from one needs. The window is
         * removed again before the taps.
         */
        private void addedWindow(final Handler h) {
            final Runnable none = new Runnable() {
                @Override public void run() {
                    log("FAIL no pre-draw for a window added with addView");
                    System.exit(7);
                }
            };
            h.postDelayed(none, 5000);
            final android.view.WindowManager wm = getWindowManager();
            final View panel = new View(this);
            panel.getViewTreeObserver().addOnPreDrawListener(
                    new ViewTreeObserver.OnPreDrawListener() {
                boolean seen;
                @Override public boolean onPreDraw() {
                    if (seen) return true;
                    seen = true;
                    h.removeCallbacks(none);
                    log("added window pre-draw sized=" + (panel.getWidth() == 50));
                    h.post(new Runnable() {
                        @Override public void run() {
                            wm.removeView(panel);
                            hitTest(h);
                        }
                    });
                    return true;
                }
            });
            wm.addView(panel, new android.view.WindowManager.LayoutParams(50, 50,
                    android.view.WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                    android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | android.view.WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE,
                    android.graphics.PixelFormat.TRANSLUCENT));
        }

        /** Last: the test taps (50,150) and (50,450) in turn. Only the second is on the moved view. */
        private void hitTest(final Handler h) {
            final Runnable noTouch = new Runnable() {
                @Override public void run() {
                    log("FAIL no touch reached the moved view where it is drawn");
                    System.exit(5);
                }
            };
            h.postDelayed(noTouch, 12000);
            // The test's taps are injected from the frame loop, which idles while
            // nothing changes: keep it turning until the touch arrives.
            h.post(new Runnable() {
                @Override public void run() {
                    if (group.onMovedTouched == null) return;
                    group.invalidate();
                    h.postDelayed(this, 50);
                }
            });
            group.onMovedTouched = new Runnable() {
                @Override public void run() {
                    group.onMovedTouched = null;
                    h.removeCallbacks(noTouch);
                    h.postDelayed(new Runnable() {
                        @Override public void run() {
                            log("FAIL the app kept running after its last Activity "
                                    + "finished");
                            System.exit(4);
                        }
                    }, 3000);
                    log("PASS");
                    finish();
                }
            };
        }
    }
}

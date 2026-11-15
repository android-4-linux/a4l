// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.opengl.GLSurfaceView,
// GLSurfaceView.Renderer, GLES20). No GPL-3.0 sources.
//
// A GLSurfaceView that is laid out again while it renders, as a game's view is when its
// window changes or the app changes its layout. The surface must survive it: the renderer
// goes on drawing and is not told its surface is gone. The renderer clears to a color and
// counts frames; the totals are logged as `GLV: ...` and the process exits.
package com.acl.glsurface;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.SurfaceHolder;
import android.widget.FrameLayout;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class Main extends Activity {
    static final String TAG = "acl";
    private volatile int frames, framesAtRelayout = -1, created, changed, destroyed;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        final FrameLayout root = new FrameLayout(this);
        final GLSurfaceView view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(2);
        view.setRenderer(new GLSurfaceView.Renderer() {
            public void onSurfaceCreated(GL10 gl, EGLConfig c) {
                created++;
                Log.i(TAG, "GLV: onSurfaceCreated " + created);
            }
            public void onSurfaceChanged(GL10 gl, int w, int h) {
                changed++;
                Log.i(TAG, "GLV: onSurfaceChanged " + w + "x" + h);
                GLES20.glViewport(0, 0, w, h);
            }
            public void onDrawFrame(GL10 gl) {
                GLES20.glClearColor(0.1f, 0.4f, 0.7f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                frames++;
            }
        });
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder h) {}
            public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}
            public void surfaceDestroyed(SurfaceHolder h) {
                destroyed++;
                Log.i(TAG, "GLV: surfaceDestroyed");
            }
        });
        root.addView(view, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);

        final Handler h = new Handler(getMainLooper());
        // Lay the view out again, twice: a changed margin, then back.
        h.postDelayed(new Runnable() {
            public void run() {
                framesAtRelayout = frames;
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) view.getLayoutParams();
                lp.topMargin = 8;
                view.setLayoutParams(lp);
                root.requestLayout();
                Log.i(TAG, "GLV: laid out again at frame " + framesAtRelayout);
            }
        }, 1500);
        h.postDelayed(new Runnable() {
            public void run() {
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) view.getLayoutParams();
                lp.topMargin = 0;
                view.setLayoutParams(lp);
            }
        }, 2000);
        h.postDelayed(new Runnable() {
            public void run() {
                Log.i(TAG, "GLV: frames " + frames + " after " + (frames - framesAtRelayout)
                        + " created " + created + " changed " + changed
                        + " destroyed " + destroyed);
                System.exit(0);
            }
        }, 3500);
    }
}

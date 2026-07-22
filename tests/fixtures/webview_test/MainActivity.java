// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl diagnostic fixture; android.webkit.WebView public API
// (developer.android.com, Apache-2.0 docs). No GPL-3.0 sources.
//
// A minimal WebView test app that isolates each WebView rendering axis in ONE page so
// a single frame dump shows which composite and which don't:
//   1. a solid-color div (pure quad compositing)
//   2. plain paragraph text (DOM raster)
//   3. a REMOTE https <img> — a real OSM raster tile (remote image load + composite)
//   4. a <canvas> painted via 2D context (accelerated canvas layer)
// Used to tell whether the openmaps blank-map bug is acl WebView compositing vs
// Leaflet-specific. Load via run-apk.sh with the usual ACL_WEBVIEW_* env.
package com.acl.webviewtest;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

public class MainActivity extends Activity {
    static final String TAG = "acl.webviewtest";

    static final String HTML =
        "<!doctype html><html><head>"
      + "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      + "<style>"
      + "  html,body{margin:0;padding:0;font:28px sans-serif;background:#fff;color:#000}"
      + "  #solid{width:100%;height:160px;background:#cc0000;color:#fff;line-height:160px;text-align:center}"
      + "  #tile{width:256px;height:256px;border:4px solid #0000ff;display:block}"
      + "  #cv{border:4px solid #ff8800}"
      + "  .lbl{padding:12px}"
      // A continuously-running CSS animation forces the WebView to keep repainting/
      // presenting every frame (a STATIC page draws once before load then idles, so
      // the content-ready invalidation never re-presents under acl's present loop).
      + "  #spin{width:80px;height:80px;margin:12px;background:#008800;"
      + "        animation:sp 1.2s linear infinite}"
      + "  @keyframes sp{from{transform:rotate(0)}to{transform:rotate(360deg)}}"
      + "</style></head><body>"
      + "<div id='solid'>SOLID RED DIV + white text</div>"
      + "<div id='spin'></div>"
      + "<p class='lbl'>Plain paragraph text (DOM raster).</p>"
      + "<div class='lbl'>Remote OSM tile &lt;img&gt; below (256x256, blue border):</div>"
      + "<img id='tile' src='https://a.tile.openstreetmap.org/12/2048/1362.png'"
      + "     onload=\"document.title='tileok'\" onerror=\"document.title='tileerr'\">"
      + "<div class='lbl'>Canvas below (orange border):</div>"
      + "<canvas id='cv' width='300' height='120'></canvas>"
      + "<script>"
      + "  var c=document.getElementById('cv').getContext('2d');"
      + "  var f=0;"
      + "  function tick(){"
      + "    f++;"
      + "    c.fillStyle='#0055ff';c.fillRect(0,0,300,120);"
      + "    c.fillStyle='#fff';c.font='24px sans-serif';c.fillText('CANVAS '+f,20,70);"
      + "    requestAnimationFrame(tick);"   // keep the compositor animating
      + "  }"
      + "  requestAnimationFrame(tick);"
      + "</script>"
      + "</body></html>";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "MainActivity.onCreate: creating WebView");
        WebView wv = new WebView(this);
        WebSettings s = wv.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        wv.setWebViewClient(new WebViewClient() {
            @Override public void onPageFinished(WebView v, String url) {
                Log.i(TAG, "WebView onPageFinished url=" + url + " title=" + v.getTitle());
            }
            @Override public void onReceivedError(WebView v, int code, String desc, String u) {
                Log.i(TAG, "WebView onReceivedError code=" + code + " desc=" + desc + " url=" + u);
            }
        });
        wv.setWebChromeClient(new android.webkit.WebChromeClient() {
            @Override public void onProgressChanged(WebView v, int p) {
                if (p == 100 || p % 25 == 0) Log.i(TAG, "WebView progress=" + p);
            }
        });
        setContentView(wv);
        // LOAD TARGET (edit + rebuild to switch): "" => inline test page (data:); http(s) => loadUrl.
        final String url = "";  // "" => inline animated test page; or an http(s) URL
        final WebView lwv = wv;
        // Load AFTER the decor is attached to the window (acl attaches post-onCreate); a load issued
        // on an unattached WebView may never dispatch its network request.
        new android.os.Handler(getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() {
                Log.i(TAG, "delayed load: attached=" + lwv.isAttachedToWindow()
                          + " winVis=" + lwv.getWindowVisibility() + " vis=" + lwv.getVisibility());
                if (url.startsWith("http")) { Log.i(TAG, "loadUrl " + url); lwv.loadUrl(url); }
                else lwv.loadDataWithBaseURL("https://openstreetmap.fr/", HTML, "text/html", "utf-8", null);
            }
        }, 1500);

        // acl's resume path is androidx-only (getLifecycle); a plain Activity leaves the WebView
        // compositor PAUSED, so it never animates/invalidates and acl presents ~4 blank frames.
        // Resume it explicitly and pump wv.invalidate() every frame so acl's present loop keeps
        // drawing (mirrors how a real animating app drives continuous presentation).
        wv.resumeTimers();
        wv.onResume();
        final WebView fwv = wv;
        // Drive acl's Choreographer-based present loop directly (the WebView's native AChoreographer
        // vsync fails to load, so its RenderThread never posts frame callbacks). Post a self-rescheduling
        // frame callback that invalidates the WebView every vsync → nTraverse runs → functor composites.
        final android.view.Choreographer ch = android.view.Choreographer.getInstance();
        ch.postFrameCallback(new android.view.Choreographer.FrameCallback() {
            int n = 0;
            @Override public void doFrame(long t) {
                fwv.invalidate();
                if (++n % 60 == 0) Log.i(TAG, "frame pump n=" + n + " title=" + fwv.getTitle());
                ch.postFrameCallback(this);
            }
        });
    }
}

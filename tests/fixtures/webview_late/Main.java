// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; android.webkit.WebView's public API
// (developer.android.com). No GPL-3.0 sources.
//
// An app with no native code of its own that makes its first WebView 500 ms after its window
// is up, and loads a page from a string. That is the order in which a WebView meets the least
// of what a4l set up at start: the app has no bionic runtime until Chromium's library is
// loaded, so there was no NDK looper to watch, no GL wired, and no thread attached to it.
//
// The page loads and its script runs (onPageFinished, onConsoleMessage), and the Activity's
// own Resources hold the WebView's resources: Chromium loads them through the Context the
// WebView was made with, and decodes one of them for the default video poster.
//
// Logs "WEBVIEW:" lines and "WEBVIEW: PASS" at the end; tests/integration/run_webview_late_test.py
// reads them.
package com.acl.wvlate;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.View;
import android.webkit.ConsoleMessage;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;

public class Main extends Activity {
    private static final String HTML = "<!doctype html><html><body>"
            + "<script>requestAnimationFrame(function(){console.log('frame');});</script>"
            + "</body></html>";

    private boolean mFinished;
    private boolean mScript;
    private boolean mDone;

    static void log(String s) {
        Log.i("acl", "WEBVIEW: " + s);
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final View first = new View(this);
        first.setBackgroundColor(Color.BLUE);
        setContentView(first);
        final Handler h = new Handler(getMainLooper());
        h.postDelayed(new Runnable() {
            @Override public void run() {
                start(h);
            }
        }, 500);
        h.postDelayed(new Runnable() {
            @Override public void run() {
                if (mDone) return;
                log("FAIL no page after 20 s: finished=" + mFinished + " script=" + mScript);
                System.exit(3);
            }
        }, 20000);
    }

    private void start(final Handler h) {
        final WebView wv = new WebView(this);
        wv.getSettings().setJavaScriptEnabled(true);
        wv.setWebViewClient(new WebViewClient() {
            @Override public void onPageFinished(WebView v, String url) {
                if (mFinished) return;
                mFinished = true;
                log("page finished");
                check();
            }
            @Override public void onReceivedError(WebView v, int code, String what, String url) {
                log("FAIL load error " + code + " " + what);
                System.exit(4);
            }
        });
        wv.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onConsoleMessage(ConsoleMessage m) {
                if (!mScript && "frame".equals(m.message())) {
                    mScript = true;
                    log("script ran");
                    check();
                }
                return true;
            }
        });
        setContentView(wv);
        wv.loadDataWithBaseURL("https://example.invalid/", HTML, "text/html", "utf-8", null);
    }

    private void check() {
        if (!mFinished || !mScript || mDone) return;
        mDone = true;
        // The drawable Chromium decodes for a video with no poster, looked up as Chromium
        // does: through this Activity's Resources.
        final int id = getResources().getIdentifier("ic_play_circle_outline_black_48dp",
                "drawable", "com.android.webview");
        log("activity resources hold the WebView's=" + (id != 0));
        final Bitmap poster = id != 0 ? BitmapFactory.decodeResource(getResources(), id) : null;
        log("default poster decoded=" + (poster != null));
        if (poster != null) log("PASS");
        finish();
    }
}

// SPDX-License-Identifier: Apache-2.0
// Provenance: original; public Android API only (android.webkit.WebView, android.widget).
// No GPL-3.0 sources.
package org.acl.internal;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/**
 * A web page shown in the app's window: what a browser tab opened over an app is on a phone.
 *
 * <p>{@link HostBootstrap#openWebLink} starts it for an ACTION_VIEW of an http or https
 * address that asks for a tab (a Custom Tabs session), which is how an app shows its sign-in
 * page. The page loads in a WebView under a bar that names the site and closes the tab.
 *
 * <p>When the page goes to an address in a scheme of the app's own, the Activity whose
 * intent filter takes that address from a browser is started with it, over this one, as a
 * browser starts it. That is how a sign-in page hands its answer back. The app's Activity
 * ends the tab the way it does on a phone, by bringing its own screen back with
 * FLAG_ACTIVITY_CLEAR_TOP. An address nothing in the app takes is not followed.
 *
 * <p>The page runs in the app's process: it does not have the separation from the app that a
 * tab in the user's browser has, and it shares none of that browser's sign-ins.
 */
public final class BrowserActivity extends Activity {
    static final String CLASS = "org.acl.internal.BrowserActivity";
    private static final String TAG = "acl";
    private static final int BAR = 0xFFF1F3F4, BAR_TEXT = 0xFF202124, PAGE = 0xFFFFFFFF;

    private final Handler mMain = new Handler(Looper.getMainLooper());
    private WebView mWeb;
    private boolean mGone;  // onDestroy has run
    private TextView mSite;
    private ProgressBar mProgress;

    @Override protected void onCreate(Bundle saved) {
        setTheme(android.R.style.Theme_Material_Light_NoActionBar);
        super.onCreate(saved);
        final Uri page = getIntent() != null ? getIntent().getData() : null;
        if (!isWeb(page)) {
            finish();
            return;
        }
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(PAGE);
        root.addView(bar(), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, px(48)));
        mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgress.setMax(100);
        root.addView(mProgress, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, px(3)));
        try {
            mWeb = new WebView(this);
        } catch (Throwable t) {
            // No WebView provider after all (ACL_WEBVIEW_APK did not load).
            Log.w(TAG, "browser: no WebView (" + t + ")");
            finish();
            return;
        }
        final WebSettings s = mWeb.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        // A page gets the web and nothing of the app's: no files, no content providers.
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setGeolocationEnabled(false);
        s.setSupportMultipleWindows(false);
        s.setJavaScriptCanOpenWindowsAutomatically(false);
        // The page is drawn in software, into a layer of the view's: the one way of drawing
        // that every WebView build has on every host. ACL_WEBVIEW_VULKAN is for a WebView
        // built to draw through a4l's Vulkan functor, and leaves it to that.
        if (System.getenv("ACL_WEBVIEW_VULKAN") == null) {
            mWeb.setLayerType(View.LAYER_TYPE_SOFTWARE, null);
        }
        mWeb.setWebViewClient(new Pages());
        mWeb.setWebChromeClient(new WebChromeClient() {
            @Override public void onProgressChanged(WebView v, int percent) {
                mProgress.setProgress(percent);
                mProgress.setVisibility(percent >= 100 ? View.INVISIBLE : View.VISIBLE);
            }
        });
        root.addView(mWeb, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);
        showSite(page);
        Log.i(TAG, "browser: opened " + site(page));
        mWeb.loadUrl(page.toString());
        mWeb.requestFocus();
    }

    // The bar: a close button, then the site the page is from.
    private View bar() {
        final LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(BAR);
        // The cross is drawn, not set as text: the fonts a host has may hold no such glyph.
        final View close = new View(this) {
            private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            @Override protected void onDraw(Canvas c) {
                mPaint.setColor(BAR_TEXT);
                mPaint.setStrokeWidth(px(2));
                mPaint.setStrokeCap(Paint.Cap.ROUND);
                final float cx = getWidth() / 2f, cy = getHeight() / 2f, r = px(7);
                c.drawLine(cx - r, cy - r, cx + r, cy + r, mPaint);
                c.drawLine(cx - r, cy + r, cx + r, cy - r, mPaint);
            }
        };
        close.setContentDescription("Close");
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                Log.i(TAG, "browser: closed");
                finish();
            }
        });
        bar.addView(close, new LinearLayout.LayoutParams(px(48), px(48)));
        mSite = new TextView(this);
        mSite.setTextColor(BAR_TEXT);
        mSite.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 15);
        mSite.setSingleLine(true);
        mSite.setEllipsize(TextUtils.TruncateAt.START);
        final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.rightMargin = px(12);
        bar.addView(mSite, lp);
        return bar;
    }

    private final class Pages extends WebViewClient {
        @Override public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
            final Uri to = r != null ? r.getUrl() : null;
            if (isWeb(to)) return false;  // the page's own navigation
            if (r != null && r.isForMainFrame()) leaveFor(to);
            return true;  // not an address a WebView loads
        }
        @Override public void onPageStarted(WebView v, String url, Bitmap icon) {
            showSite(url != null ? Uri.parse(url) : null);
        }
        @Override public void onPageFinished(WebView v, String url) {
            Log.i(TAG, "browser: loaded " + site(url != null ? Uri.parse(url) : null));
        }
        @Override public void onReceivedError(WebView v, WebResourceRequest r,
                WebResourceError e) {
            if (r == null || !r.isForMainFrame()) return;
            Log.w(TAG, "browser: " + site(r.getUrl()) + " failed ("
                    + (e != null ? e.getErrorCode() + " " + e.getDescription() : "?") + ")");
        }
    }

    // The page went to an address that is not the web's. Started on a later turn of the
    // main loop: this is called from inside the WebView, and the app's Activity may end
    // this one, and the WebView with it, as it starts.
    private void leaveFor(final Uri to) {
        if (to == null || to.getScheme() == null) return;
        final Intent view = new Intent(Intent.ACTION_VIEW, to)
                .addCategory(Intent.CATEGORY_BROWSABLE);
        final ComponentName target = HostBootstrap.ownActivityFor(view, true);
        if (target == null) {
            Log.i(TAG, "browser: nothing in the app takes " + to.getScheme() + ": addresses");
            return;
        }
        view.setComponent(target).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Log.i(TAG, "browser: " + to.getScheme() + ": address to " + target.getClassName());
        mMain.post(new Runnable() {
            @Override public void run() {
                if (!mGone) HostBootstrap.startOwnActivity(view);
            }
        });
    }

    @Override public void onBackPressed() {
        if (mWeb != null && mWeb.canGoBack()) {
            mWeb.goBack();
            return;
        }
        Log.i(TAG, "browser: closed");
        finish();
    }

    @Override protected void onResume() {
        super.onResume();
        if (mWeb != null) mWeb.onResume();
    }

    @Override protected void onPause() {
        if (mWeb != null) mWeb.onPause();
        super.onPause();
    }

    @Override protected void onDestroy() {
        mGone = true;
        if (mWeb != null) {
            final ViewGroup parent = (ViewGroup) mWeb.getParent();
            if (parent != null) parent.removeView(mWeb);
            mWeb.stopLoading();
            mWeb.destroy();
            mWeb = null;
        }
        super.onDestroy();
    }

    static boolean isWeb(Uri u) {
        final String scheme = u != null ? u.getScheme() : null;
        return "http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme);
    }

    // The site of an address: its scheme, host and port. The path and the query are left
    // out of what is shown and of what is logged: a sign-in answer is carried in them.
    static String site(Uri u) {
        if (u == null || u.getScheme() == null) return "(no address)";
        final String host = u.getHost();
        if (host == null) return u.getScheme() + ":";
        return u.getScheme() + "://" + host + (u.getPort() >= 0 ? ":" + u.getPort() : "");
    }

    private void showSite(Uri u) {
        if (mSite == null || !isWeb(u)) return;
        // https is what a sign-in page is expected on, and is not spelled out.
        final String s = site(u);
        mSite.setText("https".equalsIgnoreCase(u.getScheme())
                ? s.substring("https://".length()) : s);
    }

    private int px(int dp) {
        return Math.round(dp * getResources().getDisplayMetrics().density);
    }
}

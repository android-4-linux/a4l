// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; public Android API only (android.app.Activity,
// android.content.Intent, java.net.ServerSocket). No GPL-3.0 sources.
//
// An app that signs in through a browser tab, the way an OAuth library does it.
//
// Main is the app's screen. SignIn is the library's waiting screen: on its first onResume it
// opens the sign-in page with an ACTION_VIEW that carries a Custom Tabs session, and waits. It
// takes a later onResume with no answer to mean the user closed the tab. Redirect is the
// Activity the manifest gives the app's own scheme to (VIEW, DEFAULT, BROWSABLE): it passes
// the address it was started with to SignIn with FLAG_ACTIVITY_CLEAR_TOP and
// FLAG_ACTIVITY_SINGLE_TOP and finishes, so the answer reaches SignIn in onNewIntent, before
// the onResume that would otherwise read as a cancel.
//
// The pages come from a server the fixture runs on 127.0.0.1. Round one's page first goes to
// an address in a scheme nothing takes, then to the app's. Round two's is an HTTP redirect
// straight to the app's scheme. A second run (A4L_TEST_BROWSER=cancel) is a plain link, with
// no tab asked for, to a page of one color that goes nowhere: the test looks for the color on
// the screen and closes the tab with its button.
//
// Logs "BROWSER:" lines and "BROWSER: PASS" at the end;
// tests/integration/run_browser_tab_test.py reads them.
package com.acl.browsertab;

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.View;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

public class Main extends Activity {
    static final String SCHEME = "a4l-test-signin";
    static final String ACTION_ANSWER = "com.acl.browsertab.ANSWER";
    static final String EXTRA_PAGE = "page";
    static final String EXTRA_TAB = "tab";
    // What CustomTabsIntent puts in its intent; the value is a binder, or null.
    static final String EXTRA_SESSION = "android.support.customtabs.extra.SESSION";

    static int sPort;
    static int sRound;
    static String sAnswer;           // what SignIn ended with
    static final StringBuilder sSignIn = new StringBuilder();  // SignIn's lifecycle
    static final StringBuilder sRedirect = new StringBuilder();  // Redirect's lifecycle
    static final StringBuilder sOthers = new StringBuilder();  // Activities that are not the app's

    static void log(String s) {
        Log.i("acl", "BROWSER: " + s);
    }

    static void fail(String s) {
        log("FAIL " + s);
        System.exit(3);
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final View v = new View(this);
        v.setBackgroundColor(Color.BLUE);
        setContentView(v);
        // An app follows its Activities through these. A tab is not one of them.
        getApplication().registerActivityLifecycleCallbacks(
                new Application.ActivityLifecycleCallbacks() {
                    private void seen(Activity a) {
                        final String name = a.getClass().getName();
                        if (!name.startsWith("com.acl.browsertab.")) sOthers.append(name).append(' ');
                    }
                    @Override public void onActivityCreated(Activity a, Bundle s) { seen(a); }
                    @Override public void onActivityStarted(Activity a) { seen(a); }
                    @Override public void onActivityResumed(Activity a) { seen(a); }
                    @Override public void onActivityPaused(Activity a) { seen(a); }
                    @Override public void onActivityStopped(Activity a) { seen(a); }
                    @Override public void onActivitySaveInstanceState(Activity a, Bundle s) { }
                    @Override public void onActivityDestroyed(Activity a) { seen(a); }
                });
        try {
            sPort = serve();
        } catch (Exception e) {
            fail("no server: " + e);
        }
        new Handler(getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() {
                fail("no end after 90 s: round=" + sRound + " sign-in=" + sSignIn);
            }
        }, 90000);
    }

    @Override protected void onResume() {
        super.onResume();
        final boolean cancel = "cancel".equals(System.getenv("A4L_TEST_BROWSER"));
        if (sRound > 0) {
            if ("none".equals(sAnswer)) fail("back on the main screen with no outcome");
            log("round " + sRound + " " + sAnswer + " sign-in=" + sSignIn
                    + " redirect=" + sRedirect + " others=[" + sOthers.toString().trim() + "]");
        }
        if (sRound == (cancel ? 1 : 2)) {
            log("PASS");
            finish();
            System.exit(0);
        }
        sRound++;
        sAnswer = "none";
        sSignIn.setLength(0);
        sRedirect.setLength(0);
        final String page = cancel ? "/wait" : sRound == 1 ? "/start" : "/direct";
        // Started as a tap on a button starts it: not from inside this screen's own launch.
        new Handler(getMainLooper()).post(new Runnable() {
            @Override public void run() {
                startActivity(new Intent(Main.this, SignIn.class)
                        .putExtra(EXTRA_PAGE, "http://127.0.0.1:" + sPort + page)
                        .putExtra(EXTRA_TAB, !cancel));
            }
        });
    }

    /** The library's waiting screen. */
    public static class SignIn extends Activity {
        private boolean mAsked;  // the tab has been opened and no answer has come

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            sSignIn.append('c');
            final View v = new View(this);
            v.setBackgroundColor(Color.GREEN);
            setContentView(v);
        }

        @Override protected void onNewIntent(Intent i) {
            super.onNewIntent(i);
            sSignIn.append('n');
            if (!ACTION_ANSWER.equals(i.getAction()) || i.getData() == null) return;
            mAsked = false;
            final Uri d = i.getData();
            sAnswer = "answer code=" + d.getQueryParameter("code")
                    + " state=" + d.getQueryParameter("state");
            finish();
        }

        @Override protected void onResume() {
            super.onResume();
            sSignIn.append('r');
            if (isFinishing()) return;
            if (mAsked) {
                // Back with nothing: the user closed the tab.
                mAsked = false;
                sAnswer = "cancelled";
                finish();
                return;
            }
            mAsked = true;
            final Intent view = new Intent(Intent.ACTION_VIEW,
                    Uri.parse(getIntent().getStringExtra(EXTRA_PAGE)));
            if (getIntent().getBooleanExtra(EXTRA_TAB, true)) {
                final Bundle session = new Bundle();
                session.putBinder(EXTRA_SESSION, null);
                view.putExtras(session);
            }
            startActivity(view);
        }

        @Override protected void onPause() {
            super.onPause();
            sSignIn.append('p');
        }

        @Override protected void onStop() {
            super.onStop();
            sSignIn.append('s');
        }

        @Override protected void onDestroy() {
            super.onDestroy();
            sSignIn.append('d');
        }
    }

    /** Takes the app's own scheme from a browser and passes the address on. */
    public static class Redirect extends Activity {
        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            final Intent in = getIntent();
            sRedirect.append('c');
            log("redirect started with " + in.getData().getScheme() + " browsable="
                    + in.hasCategory(Intent.CATEGORY_BROWSABLE));
            startActivity(new Intent(this, SignIn.class)
                    .setAction(ACTION_ANSWER)
                    .setData(in.getData())
                    .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP));
            finish();
        }

        // It finishes in onCreate: it is destroyed, and not started, resumed, paused or
        // stopped on the way.
        @Override protected void onStart() {
            super.onStart();
            sRedirect.append('S');
        }

        @Override protected void onResume() {
            super.onResume();
            sRedirect.append('r');
        }

        @Override protected void onPause() {
            super.onPause();
            sRedirect.append('p');
        }

        @Override protected void onStop() {
            super.onStop();
            sRedirect.append('s');
        }

        @Override protected void onDestroy() {
            super.onDestroy();
            sRedirect.append('d');
        }
    }

    /** Declared with a filter for a scheme of its own, and not exported: a page cannot start it. */
    public static class Private extends Activity {
        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            fail("a page started an Activity that is not exported");
        }
    }

    // The sign-in site: one request a connection.
    static int serve() throws Exception {
        final ServerSocket server = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
        final Thread t = new Thread(new Runnable() {
            @Override public void run() {
                while (true) {
                    try (Socket s = server.accept()) {
                        answer(s);
                    } catch (Exception e) {
                        log("server: " + e);
                    }
                }
            }
        }, "signin-site");
        t.setDaemon(true);
        t.start();
        return server.getLocalPort();
    }

    static void answer(Socket s) throws Exception {
        final BufferedReader in = new BufferedReader(
                new InputStreamReader(s.getInputStream(), StandardCharsets.US_ASCII));
        final String request = in.readLine();
        for (String line = in.readLine(); line != null && !line.isEmpty(); line = in.readLine()) { }
        final String[] parts = request != null ? request.split(" ") : new String[0];
        final String path = parts.length > 1 ? parts[1] : "";
        final String app = SCHEME + "://callback?code=";
        String status = "200 OK", location = null, body = "";
        if (path.startsWith("/start")) {
            status = "302 Found";
            location = "/login";
        } else if (path.startsWith("/login")) {
            // An address nothing takes, one an Activity that is not exported takes, then
            // the app's.
            body = "<!doctype html><html><body><p>Signing in</p><script>"
                    + "location.href='a4l-test-nobody://x';"
                    + "setTimeout(function(){location.href='a4l-test-private://x';},200);"
                    + "setTimeout(function(){location.href='" + app + "one&state=r1';},400);"
                    + "</script></body></html>";
        } else if (path.startsWith("/direct")) {
            status = "302 Found";
            location = app + "two&state=r2";
        } else if (path.startsWith("/wait")) {
            // A page of one color, which the test looks for on the screen.
            body = "<!doctype html><html><body style=\"margin:0;background:#2060c0\">"
                    + "</body></html>";
        } else {
            status = "404 Not Found";
        }
        log("site " + path + " -> " + status);
        final byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        final String head = "HTTP/1.1 " + status + "\r\n"
                + (location != null ? "Location: " + location + "\r\n" : "")
                + "Content-Type: text/html; charset=utf-8\r\n"
                + "Content-Length: " + bytes.length + "\r\n"
                + "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
        final OutputStream out = s.getOutputStream();
        out.write(head.getBytes(StandardCharsets.US_ASCII));
        out.write(bytes);
        out.flush();
    }
}

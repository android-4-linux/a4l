// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; public Android API only (android.app.Activity,
// android.content.Intent, java.net.ServerSocket). No GPL-3.0 sources.
//
// An app that signs in through a browser tab, the way an OAuth library does it.
//
// Splash is the launch screen, which starts Main and finishes. Main is the app's screen; it
// first starts itself again with FLAG_ACTIVITY_SINGLE_TOP, to be paused, given the Intent and
// resumed. SignIn is the library's waiting screen: on its first onResume it
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
// the screen, types into the page's text field, and closes the tab with its button. A third
// run (A4L_TEST_BROWSER=silent) is two rounds of that plain link on a host whose portal
// never answers: the link is tapped again while nothing has opened, and round two also
// sends an email and shares a link. None of it may hold the main loop. A fourth run
// (A4L_TEST_BROWSER=host) is the plain link on a host that takes it: no tab comes up, and
// the waiting screen ends the round itself.
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
    // SignIn's ActivityLifecycleCallbacks in the order they came: c, s, r, p, t and d for
    // created, started, resumed, paused, stopped and destroyed, "<c" for the one before it
    // and "c>" for the one after.
    static final StringBuilder sCallbacks = new StringBuilder();
    // The app's started and resumed Activities, counted the way androidx's
    // ProcessLifecycleOwner counts them: up in onActivityPostStarted and
    // onActivityPostResumed, on callbacks it registers on each Activity in
    // onActivityPreCreated, and down in onActivityStopped and onActivityPaused.
    static int sStarted, sResumed;
    static boolean sAgain;  // Main has been started again while it was the top Activity
    static final String RUN = System.getenv("A4L_TEST_BROWSER");
    static final boolean CANCEL = "cancel".equals(RUN);
    static final boolean SILENT = "silent".equals(RUN);
    static final boolean HOST = "host".equals(RUN);
    static final boolean PLAIN = CANCEL || SILENT || HOST;  // a link with no tab asked for
    // The longest a startActivity for something of the host's held the main loop this
    // round, when the link was first asked for, and how long after that the tab was up.
    static long sHeld, sAskedAt, sTabUp;
    static final StringBuilder sMain = new StringBuilder();  // Main's lifecycle from then on

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

    @Override protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        sMain.append('n');
    }

    @Override protected void onPause() {
        super.onPause();
        sMain.append('p');
    }

    @Override protected void onResume() {
        super.onResume();
        if (!sAgain) {
            // First, started again while it is the top Activity, with
            // FLAG_ACTIVITY_SINGLE_TOP: it is paused, given the Intent and resumed.
            sAgain = true;
            new Handler(getMainLooper()).post(new Runnable() {
                @Override public void run() {
                    startActivity(new Intent(Main.this, Main.class)
                            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP));
                }
            });
            return;
        }
        sMain.append('r');
        if (sRound == 0) log("started again on top: " + sMain);
        if (sRound > 0) {
            if ("none".equals(sAnswer)) fail("back on the main screen with no outcome");
            log("round " + sRound + " " + sAnswer + " sign-in=" + sSignIn
                    + " redirect=" + sRedirect + " others=[" + sOthers.toString().trim() + "]");
            log("round " + sRound + " callbacks=" + sCallbacks.toString().trim());
        }
        if (sRound == (CANCEL || HOST ? 1 : 2)) {
            log("PASS");
            finish();
            System.exit(0);
        }
        sRound++;
        sAnswer = "none";
        sSignIn.setLength(0);
        sRedirect.setLength(0);
        sCallbacks.setLength(0);
        final String page = PLAIN ? "/wait" : sRound == 1 ? "/start" : "/direct";
        // Started as a tap on a button starts it: not from inside this screen's own launch.
        // A little later, so that the launch screen, which finished as it started this one,
        // is gone.
        new Handler(getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() {
                // This screen is up and nothing else is: one started, one resumed.
                log("round " + sRound + " starts with started=" + sStarted
                        + " resumed=" + sResumed);
                // A key an input method sends goes to the window's root through its handler,
                // as a key a WebView hands back does. The root has to take it without
                // throwing on the main loop.
                if (sRound == 1) {
                    String sent;
                    try {
                        sent = String.valueOf(new android.view.inputmethod.BaseInputConnection(
                                getWindow().getDecorView(), false).sendKeyEvent(
                                        new android.view.KeyEvent(
                                                android.view.KeyEvent.ACTION_DOWN,
                                                android.view.KeyEvent.KEYCODE_A)));
                    } catch (Throwable t) {
                        sent = t.toString();
                    }
                    log("key from an input method sent=" + sent);
                }
                startActivity(new Intent(Main.this, SignIn.class)
                        .putExtra(EXTRA_PAGE, "http://127.0.0.1:" + sPort + page)
                        .putExtra(EXTRA_TAB, !PLAIN));
            }
        }, 300);
    }

    /**
     * The app's Application. It follows the app's Activities through these callbacks, from
     * before the first one is created. A tab is not one of them.
     */
    public static class App extends Application {
        @Override public void onCreate() {
            super.onCreate();
            registerActivityLifecycleCallbacks(new Followed());
        }
    }

    static final class Followed implements Application.ActivityLifecycleCallbacks {
        private void seen(Activity a) {
            final String name = a.getClass().getName();
            if (!name.startsWith("com.acl.browsertab.")) sOthers.append(name).append(' ');
        }
        private void at(Activity a, String step) {
            seen(a);
            if (a instanceof SignIn) sCallbacks.append(step).append(' ');
        }
        @Override public void onActivityPreCreated(Activity a, Bundle s) {
            at(a, "<c");
            a.registerActivityLifecycleCallbacks(new Counted());
        }
        @Override public void onActivityCreated(Activity a, Bundle s) { at(a, "c"); }
        @Override public void onActivityPostCreated(Activity a, Bundle s) { at(a, "c>"); }
        @Override public void onActivityPreStarted(Activity a) { at(a, "<s"); }
        @Override public void onActivityStarted(Activity a) { at(a, "s"); }
        @Override public void onActivityPostStarted(Activity a) { at(a, "s>"); }
        @Override public void onActivityPreResumed(Activity a) { at(a, "<r"); }
        @Override public void onActivityResumed(Activity a) { at(a, "r"); }
        @Override public void onActivityPostResumed(Activity a) { at(a, "r>"); }
        @Override public void onActivityPrePaused(Activity a) { at(a, "<p"); }
        @Override public void onActivityPaused(Activity a) {
            at(a, "p");
            sResumed--;
        }
        @Override public void onActivityPostPaused(Activity a) { at(a, "p>"); }
        @Override public void onActivityPreStopped(Activity a) { at(a, "<t"); }
        @Override public void onActivityStopped(Activity a) {
            at(a, "t");
            sStarted--;
        }
        @Override public void onActivityPostStopped(Activity a) { at(a, "t>"); }
        @Override public void onActivitySaveInstanceState(Activity a, Bundle s) { }
        @Override public void onActivityPreDestroyed(Activity a) { at(a, "<d"); }
        @Override public void onActivityDestroyed(Activity a) { at(a, "d"); }
        @Override public void onActivityPostDestroyed(Activity a) {
            at(a, "d>");
            // A callback that throws is the app's fault, and is not to stop the lifecycle:
            // the launch screen's last one does, and the run goes on.
            if (a instanceof Splash) {
                throw new IllegalStateException("thrown by the app's callback");
            }
        }
    }

    /**
     * The app's launch screen, which starts the main screen and finishes. In the first run
     * it does so once it is up: it is paused and stopped as the main screen covers it, and
     * then destroyed, with no second onPause or onStop. In the second run
     * (A4L_TEST_BROWSER=cancel) it does so from inside its own onCreate, as a screen that
     * only routes does.
     */
    public static class Splash extends Activity {
        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            if (CANCEL) {
                startActivity(new Intent(this, Main.class));
                finish();
                return;
            }
            new Handler(getMainLooper()).post(new Runnable() {
                @Override public void run() {
                    startActivity(new Intent(Splash.this, Main.class));
                    finish();
                }
            });
        }
    }

    /** Counts an Activity started and resumed; registered on it in onActivityPreCreated. */
    static final class Counted implements Application.ActivityLifecycleCallbacks {
        @Override public void onActivityCreated(Activity a, Bundle s) { }
        @Override public void onActivityStarted(Activity a) { }
        @Override public void onActivityPostStarted(Activity a) { sStarted++; }
        @Override public void onActivityResumed(Activity a) { }
        @Override public void onActivityPostResumed(Activity a) { sResumed++; }
        @Override public void onActivityPaused(Activity a) { }
        @Override public void onActivityStopped(Activity a) { }
        @Override public void onActivitySaveInstanceState(Activity a, Bundle s) { }
        @Override public void onActivityDestroyed(Activity a) { }
    }

    /** The library's waiting screen. */
    public static class SignIn extends Activity {
        private boolean mAsked;  // the tab has been opened and no answer has come
        private boolean mResumed;

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
                if (SILENT) {
                    log("round " + sRound + " link held the main loop "
                            + (sHeld < 1000 ? "under a second" : sHeld + " ms") + ", tab up "
                            + (sTabUp >= 4000 ? "after the host's timeout"
                                    : sTabUp >= 0 && sTabUp < 3000 ? "at once"
                                    : "in " + sTabUp + " ms"));
                }
                finish();
                return;
            }
            mAsked = true;
            mResumed = true;
            final Intent view = new Intent(Intent.ACTION_VIEW,
                    Uri.parse(getIntent().getStringExtra(EXTRA_PAGE)));
            if (getIntent().getBooleanExtra(EXTRA_TAB, true)) {
                final Bundle session = new Bundle();
                session.putBinder(EXTRA_SESSION, null);
                view.putExtras(session);
            }
            sHeld = 0;
            sTabUp = -1;
            if (SILENT && sRound == 2) {
                // An email and a shared link are the host's to take too.
                final long from = android.os.SystemClock.uptimeMillis();
                startActivity(new Intent(Intent.ACTION_SENDTO,
                        Uri.parse("mailto:nobody@example.com?subject=a4l")));
                startActivity(Intent.createChooser(new Intent(Intent.ACTION_SEND)
                        .setType("text/plain")
                        .putExtra(Intent.EXTRA_TEXT, "http://127.0.0.1:" + sPort + "/shared"),
                        null));
                final long held = android.os.SystemClock.uptimeMillis() - from;
                log("round 2 email and share held the main loop "
                        + (held < 1000 ? "under a second" : held + " ms"));
            }
            // The link again, twice, while nothing has opened: a user who taps it again.
            // Queued before the first ask, as a tap is that comes while the main loop is
            // held.
            for (int i = 1; SILENT && i <= 2; i++) {
                new Handler(getMainLooper()).postDelayed(new Runnable() {
                    @Override public void run() {
                        if (mResumed && !isFinishing()) open(new Intent(view));
                    }
                }, 300 * i);
            }
            sAskedAt = android.os.SystemClock.uptimeMillis();
            open(view);
            if (!HOST) return;
            // The host takes the link: nothing of the app's comes up over this screen.
            new Handler(getMainLooper()).postDelayed(new Runnable() {
                @Override public void run() {
                    if (!mResumed || isFinishing()) return;
                    mAsked = false;
                    sAnswer = "left to the host";
                    log("round " + sRound + " link held the main loop "
                            + (sHeld < 1000 ? "under a second" : sHeld + " ms")
                            + ", no tab");
                    finish();
                }
            }, 2000);
        }

        private void open(Intent view) {
            final long from = android.os.SystemClock.uptimeMillis();
            startActivity(view);
            sHeld = Math.max(sHeld, android.os.SystemClock.uptimeMillis() - from);
        }

        @Override protected void onPause() {
            super.onPause();
            sSignIn.append('p');
            mResumed = false;
            if (mAsked && sTabUp < 0) sTabUp = android.os.SystemClock.uptimeMillis() - sAskedAt;
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
            // A page of one color, which the test looks for on the screen. The text field
            // in its corner tells the site what it holds each time that changes.
            body = "<!doctype html><html><body style=\"margin:0;background:#2060c0\">"
                    + "<input id=\"t\" style=\"position:absolute;left:0;top:0;width:200px;"
                    + "height:60px;box-sizing:border-box\">"
                    + "<script>var t=document.getElementById('t');"
                    + "t.addEventListener('input',function(){"
                    + "fetch('/typed?v='+encodeURIComponent(t.value));});</script>"
                    + "</body></html>";
        } else if (path.startsWith("/typed")) {
            // What the page's text field holds; the line logged below carries it.
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

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/browser_tab/Main.java. No GPL-3.0
# sources.
#
# A sign-in through a browser tab, start to finish. The fixture is an app that opens its
# sign-in page the way an OAuth library does: an ACTION_VIEW of a web address with a Custom
# Tabs session, from a waiting screen that reads a resume with no answer as a cancel. a4l
# shows the page in the app's window (org.acl.internal.BrowserActivity). When the page goes to
# an address in the app's own scheme, the app's redirect Activity is started with it and hands
# it to the waiting screen with FLAG_ACTIVITY_CLEAR_TOP, which ends the tab.
#
# Run one, two rounds:
#   1. the page is reached through an HTTP redirect, and its script goes to an address in a
#      scheme nothing takes, then to one only an Activity that is not exported takes, then to
#      the app's. The first two are not followed.
#   2. the page is an HTTP redirect straight to the app's scheme.
# In both the waiting screen gets the answer in onNewIntent before it is resumed, the redirect
# Activity, cleared from inside its own onCreate, is destroyed and not started, and the app's
# ActivityLifecycleCallbacks are not told of the tab's Activity.
#
# The app follows its Activities the way androidx's ProcessLifecycleOwner does, and the test
# checks what it saw. The waiting screen's callbacks come in Android's order, each lifecycle
# method's between the one before it and the one after (onActivityPreCreated,
# onActivityCreated, onActivityPostCreated, and so on), each once. And when a round starts
# the app counts one Activity started and one resumed: its launch screen, which started the
# main screen and finished, got no onPause or onStop too many on the way out, and the main
# screen, started again while on top (FLAG_ACTIVITY_SINGLE_TOP), was paused for onNewIntent
# and resumed after it. The launch screen starts the main screen once it is up in run one,
# and from inside its own onCreate in run two.
#
# Run two (A4L_TEST_BROWSER=cancel, ACL_BROWSER=app): a plain link, with no tab asked for, to
# a page of one color that goes nowhere. The test finds the tab's bar and the page's color on
# a headless Weston's screen, with the AOSP prebuilt WebView and nothing set for it. A tap on
# the page's text field and two keys put "hi" in it, which the page's script tells the site.
# The WebView hands the root each key event the page did not take, and that must not throw on
# the main loop. Then a tap on the tab's close button ends the tab, and the waiting screen is
# resumed with no answer.
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain, a WebView APK (ACL_WEBVIEW_APK, or
# the AOSP prebuilt under external/chromium-webview), a render node, and Weston with its kiosk
# shell and weston-screenshooter. Exits 77 (skip) where any of these is missing.
#
# Usage: run_browser_tab_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from run_gl_surfaceview_test import H, TOLERANCE, W, first, shot, start_weston
from run_video_plane_test import skip

SKIP = 77

PACKAGE = "com.acl.browsertab"

# The waiting screen's lifecycle in a round that ends with an answer: created and resumed,
# paused and stopped under the tab, then the answer (n) before the resume, and gone. The
# redirect screen, which finishes in onCreate, is created and destroyed (cd) and no more.
ANSWERED = "crpsnrpsd"

# The waiting screen's ActivityLifecycleCallbacks over a round, as Android gives them: each
# lifecycle method's callback between the one before it ("<c") and the one after ("c>").
# Created, started and resumed; paused and stopped under the tab; started and resumed again
# when the tab is gone, with an answer or without; paused, stopped and destroyed.
UP = "<s s s> <r r r>"
DOWN = "<p p p> <t t t>"
ANSWERED_CALLBACKS = f"<c c c> {UP} {DOWN} {UP} {DOWN} <d d d>"
# One Activity of the app's is started and resumed when a round starts, counted the way
# androidx's ProcessLifecycleOwner counts: an app reads that as being in the foreground.
FOREGROUND = "started=1 resumed=1"
# The main screen's lifecycle when it is started again on top: onPause, onNewIntent, onResume.
AGAIN = "BROWSER: started again on top: pnr"
# The fixture's last callback for its launch screen throws. a4l logs it and goes on.
THREW = ("dispatchActivityPostDestroyed on com.acl.browsertab.Main$Splash: "
         "java.lang.IllegalStateException: thrown by the app's callback")

# A key from an input method is taken by the window's root. Without the fields its queue
# needs, the root threw on the main loop: "FATAL EXCEPTION" in the log fails a run.
IME_KEY = "BROWSER: key from an input method sent=false"

REDIRECT_CHECKS = [
    AGAIN,
    THREW,
    IME_KEY,
    "BROWSER: site /start -> 302 Found",
    "BROWSER: site /login -> 200 OK",
    "browser: nothing in the app takes a4l-test-nobody: addresses",
    "browser: nothing in the app takes a4l-test-private: addresses",
    "BROWSER: redirect started with a4l-test-signin browsable=true",
    f"BROWSER: round 1 starts with {FOREGROUND}",
    f"BROWSER: round 1 answer code=one state=r1 sign-in={ANSWERED} redirect=cd others=[]",
    f"BROWSER: round 1 callbacks={ANSWERED_CALLBACKS}",
    "BROWSER: site /direct -> 302 Found",
    f"BROWSER: round 2 starts with {FOREGROUND}",
    f"BROWSER: round 2 answer code=two state=r2 sign-in={ANSWERED} redirect=cd others=[]",
    f"BROWSER: round 2 callbacks={ANSWERED_CALLBACKS}",
    "BROWSER: PASS",
]

# Closed by hand: the waiting screen is resumed (r) with no answer before it.
CANCEL_CHECKS = [
    AGAIN,
    THREW,
    IME_KEY,
    f"BROWSER: round 1 starts with {FOREGROUND}",
    "BROWSER: site /wait -> 200 OK",
    "BROWSER: site /typed?v=hi -> 200 OK",
    "browser: closed",
    "BROWSER: round 1 cancelled sign-in=crpsrpsd redirect= others=[]",
    f"BROWSER: round 1 callbacks={ANSWERED_CALLBACKS}",
    "BROWSER: PASS",
]

# The tab on the screen: its bar, 48 dp high, and under it the page, which is one color.
BAR = ((W // 2, 24), (241, 243, 244))
PAGE = ((W // 2, H // 2), (32, 96, 192))

# The tab's close button is the first 48 dp of its bar. Tapped every 1.5 s from 20 s after
# the app starts, which leaves the test the time before that to find the page on the screen.
CLOSE_FROM_MS = 20000
CLOSE_TAPS = ";".join(f"20,20,{ms}" for ms in range(CLOSE_FROM_MS, 90000, 1500))

# The page's text field is its first 200x60 px, under the bar and the progress line. Tapped a
# few times before the keys, in case the page is late: a tap on a field that has the focus
# changes nothing.
FIELD_TAPS = ";".join(f"100,80,{ms}" for ms in range(10000, 17000, 1500))
KEYS = "104,105"  # h, i
KEYS_AT_MS = 17500


def story_of(out):
    return "\n".join(ln for ln in out.splitlines()
                     if "BROWSER: " in ln or "browser: " in ln or "acl WebView" in ln
                     or "activity-stack" in ln or "Fatal signal" in ln
                     or "dispatchActivity" in ln or "FATAL EXCEPTION" in ln
                     or "input: synthetic KEYS" in ln)


def painted(display, work):
    """Whether the screen shows the tab's bar over the page's color; and what it shows."""
    image = shot(display, work)
    if image is None:
        return False, "no screenshot"
    w, h, px = image
    if (w, h) != (W, H):
        return False, f"the screen is {w}x{h}, not {W}x{H}"
    wrong = []
    for name, ((x, y), want) in (("bar", BAR), ("page", PAGE)):
        got = tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
        if any(abs(g - c) > TOLERANCE for g, c in zip(got, want)):
            wrong.append(f"{name} at {x},{y} is {got}, not {want}")
    return not wrong, "; ".join(wrong)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_browser_tab_test.py <a4l> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not runtime_dir or not os.path.isdir(runtime_dir):
        return skip("XDG_RUNTIME_DIR not set")
    if not os.path.isfile(android_jar):
        return skip("no staged android.jar")
    if not glob.glob("/dev/dri/renderD*"):
        return skip("no render node")
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    webview = os.environ.get("ACL_WEBVIEW_APK", "") or os.path.join(
        root, "external/chromium-webview/prebuilt/x86_64/webview.apk")
    if not os.path.isfile(webview):
        return skip("no WebView APK")
    d8 = os.path.join(home, "bin", "d8")
    zipalign = first(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            return skip(f"{name} not found")

    def run(cmd, **kw):
        subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

    fixtures = os.path.join(src_root, "tests/fixtures")
    work = tempfile.mkdtemp(prefix="a4l-browser-")
    weston = app = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        appdir = os.path.join(work, "app")
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        for d in (appdir, cls, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "browser_tab/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(
                utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main$Splash",
                application=f"{PACKAGE}.Main$App",
                permissions=["android.permission.INTERNET"],
                activities=[
                    (f"{PACKAGE}.Main", None),
                    (f"{PACKAGE}.Main$SignIn", None),
                    (f"{PACKAGE}.Main$Redirect", None,
                     {"exported": True, "view": "a4l-test-signin"}),
                    (f"{PACKAGE}.Main$Private", None,
                     {"exported": False, "view": "a4l-test-private"}),
                ]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "browser-tab.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)

        def environment(tag, extra):
            env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="120", ACL_BIN=acl,
                       ACL_PRESENT_GPU="1", ACL_WEBVIEW_APK=webview,
                       XDG_CACHE_HOME=os.path.join(work, tag, "cache"),
                       XDG_DATA_HOME=os.path.join(work, tag, "data"))
            # What would change which path the WebView takes, or where a link goes.
            for name in ("ACL_GL_FORWARD", "ACL_WEBVIEW_VULKAN", "ACL_WEBVIEW_ARGS",
                         "ACL_BROWSER"):
                env.pop(name, None)
            env.update(extra)
            return env

        command = [os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off", "--present"]

        # Run one: the answer comes back.
        p = subprocess.run(command, env=environment("redirect", {}), capture_output=True,
                           text=True, errors="replace", timeout=300)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        if "BROWSER: FAIL" in out:
            sys.stderr.write("FAIL (redirect): the fixture reported a failure\n"
                             f"{story_of(out)}\n{out[-4000:]}\n")
            return 1
        if "FATAL EXCEPTION" in out:
            sys.stderr.write("FAIL (redirect): an exception on the main loop\n"
                             f"{story_of(out)}\n{out[-4000:]}\n")
            return 1
        for c in REDIRECT_CHECKS:
            if c not in out:
                sys.stderr.write(f"FAIL (redirect): missing '{c}'\n{story_of(out)}\n"
                                 f"{out[-4000:]}\n")
                return 1
        sys.stdout.write(f"redirect:\n{story_of(out)}\n")

        # Run two: the page is on the screen, and the tab is closed by its button.
        started = time.monotonic()
        app = subprocess.Popen(
            command, env=environment("cancel", {
                "A4L_TEST_BROWSER": "cancel", "ACL_BROWSER": "app", "ACL_TEST_MS": "1",
                "ACL_TEST_TAP": FIELD_TAPS + ";" + CLOSE_TAPS,
                "ACL_TEST_KEYS": KEYS, "ACL_TEST_KEYS_AT": str(KEYS_AT_MS)}),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
            start_new_session=True)
        lines = []
        loaded = threading.Event()
        ended = threading.Event()

        def read():
            for line in app.stdout:
                lines.append(line)
                if "browser: loaded" in line:
                    loaded.set()
            loaded.set()
            ended.set()

        threading.Thread(target=read, daemon=True).start()
        loaded.wait(60)
        shown, why = False, "the page did not load"
        # Until shortly before the first tap on the close button.
        while not ended.is_set() and time.monotonic() - started < CLOSE_FROM_MS / 1000 - 2:
            shown, why = painted(display, work)
            if shown:
                break
            time.sleep(0.5)
        ended.wait(120)
        out = "".join(lines)
        if not shown:
            sys.stderr.write(f"FAIL (cancel): the page was not on the screen: {why}\n"
                             f"{story_of(out)}\n{out[-4000:]}\n")
            return 1
        if "BROWSER: FAIL" in out:
            sys.stderr.write("FAIL (cancel): the fixture reported a failure\n"
                             f"{story_of(out)}\n{out[-4000:]}\n")
            return 1
        if "FATAL EXCEPTION" in out:
            at = out.index("FATAL EXCEPTION")
            sys.stderr.write("FAIL (cancel): an exception on the main loop\n"
                             f"{story_of(out)}\n{out[at:at + 1500]}\n")
            return 1
        for c in CANCEL_CHECKS:
            if c not in out:
                sys.stderr.write(f"FAIL (cancel): missing '{c}'\n{story_of(out)}\n"
                                 f"{out[-4000:]}\n")
                return 1
        sys.stdout.write(f"cancel:\n{story_of(out)}\n")
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired as e:
        sys.stderr.write(f"FAIL: the app did not end: {e}\n")
        return 1
    finally:
        for proc in (app, weston):
            if proc is not None and proc.poll() is None:
                try:
                    os.killpg(proc.pid, 15)
                    proc.wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    proc.kill()
        shutil.rmtree(work, ignore_errors=True)
    sys.stdout.write("OK: two sign-ins answered through a tab in the app's window; a third "
                     "tab's page on the screen, typed into, and closed by its button\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

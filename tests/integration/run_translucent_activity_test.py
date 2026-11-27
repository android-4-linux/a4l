# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/translucent_activity/Main.java and
# tests/fixtures/gl_surface_under_views/Main.java; the Weston helpers of
# run_gl_surfaceview_test.py. No GPL-3.0 sources.
#
# An Activity with a translucent theme over another: a welcome card over a map. a4l drew
# only the Activity on top, so the card came up on black, and the map under it was told
# its surface was gone.
#
# The fixture's Map lays a GL view (green and blue halves, a light band along the bottom)
# under a red banner, and starts Welcome over itself. Welcome's window is clear but for a
# yellow card in the middle and a half-black strip down the right edge. The test starts a headless Weston with its kiosk shell, runs the fixture
# with --present, and steps it through a control file, reading a screenshot and the
# fixture's lifecycle log at each step:
#
#   1. Welcome over Map: Map shows through, paused and not stopped, and its GL view has a
#      surface and draws.
#   2. Welcome finished: Map is resumed without a restart, and keeps its surface.
#   3. Welcome again, with the window's dim on: Map shows through at half brightness.
#   4. An opaque Activity over Welcome: both under it are stopped and the surface goes.
#   5. That one finished: Welcome is resumed, and Map under it is started again, not
#      resumed, and its GL view gets a new surface there.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, Weston with its kiosk shell and
# weston-screenshooter, and a render node. Exits 77 (skip) where any of these is missing.
#
# Usage: run_translucent_activity_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from run_gl_surfaceview_test import BANNER, H, TOLERANCE, W, first, shot, start_weston
from run_video_plane_test import skip

THEME_TRANSLUCENT_NO_TITLE_BAR = 0x01030010  # android.R.style.Theme_Translucent_NoTitleBar

RED, GREEN, BLUE = (0xC0, 0x20, 0x20), (0x20, 0xC0, 0x20), (0x20, 0x20, 0xC0)
LIGHT, CARD, COVER = (0xE0, 0xE0, 0xE0), (0xE0, 0xC0, 0x20), (0xC0, 0x20, 0xC0)


def dimmed(color, times=1):
    """A color under one or two layers of half black."""
    return tuple(round(c * 0.5 ** times) for c in color)


# Welcome's card is 200x100 in the middle, and its strip the rightmost 200 pixels.
BANNER_AT, LEFT_AT, RIGHT_AT = (200, BANNER // 2), (200, 200), (550, 200)
BOTTOM_AT, CARD_AT = (200, H - 40), (W // 2 + 50, H // 2)
STRIP_BANNER_AT, STRIP_MAP_AT = (700, BANNER // 2), (700, 200)

MAP_ALONE = {
    "banner": (BANNER_AT, RED), "left": (LEFT_AT, GREEN), "right": (RIGHT_AT, BLUE),
    "bottom": (BOTTOM_AT, LIGHT), "where the card was": (CARD_AT, BLUE),
    "where the strip was": (STRIP_MAP_AT, BLUE),
}
UNDER_WELCOME = {
    "banner": (BANNER_AT, RED), "left": (LEFT_AT, GREEN), "right": (RIGHT_AT, BLUE),
    "bottom": (BOTTOM_AT, LIGHT), "card": (CARD_AT, CARD),
    "strip over the banner": (STRIP_BANNER_AT, dimmed(RED)),
    "strip over the map": (STRIP_MAP_AT, dimmed(BLUE)),
}
UNDER_DIMMED_WELCOME = {
    "banner": (BANNER_AT, dimmed(RED)), "left": (LEFT_AT, dimmed(GREEN)),
    "right": (RIGHT_AT, dimmed(BLUE)), "bottom": (BOTTOM_AT, dimmed(LIGHT)),
    "card": (CARD_AT, CARD),
    "strip over the banner": (STRIP_BANNER_AT, dimmed(RED, 2)),
    "strip over the map": (STRIP_MAP_AT, dimmed(BLUE, 2)),
}
COVERED = {
    "banner": (BANNER_AT, COVER), "left": (LEFT_AT, COVER), "card": (CARD_AT, COVER),
    "strip": (STRIP_MAP_AT, COVER),
}


def mismatches(image, want):
    if image is None:
        return ["no screenshot"]
    w, h, px = image
    if (w, h) != (W, H):
        return [f"the screen is {w}x{h}, not {W}x{H}"]
    wrong = []
    for name, ((x, y), color) in want.items():
        got = tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
        if any(abs(g - c) > TOLERANCE for g, c in zip(got, color)):
            wrong.append(f"{name} at {x},{y} is {got}, not {color}")
    return wrong


class Fixture:
    """The running fixture: its log, and the control file that steps it."""

    def __init__(self, app, control):
        self.app = app
        self.control = control
        self.lines = []
        self.lock = threading.Condition()
        self.sent = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.app.stdout:
            with self.lock:
                self.lines.append(line)
                self.lock.notify_all()
        with self.lock:
            self.lines.append("")  # end of output
            self.lock.notify_all()

    def count(self, text):
        with self.lock:
            return sum(1 for line in self.lines if text in line)

    def wait(self, text, times, timeout=30):
        """True once `text` has been logged `times` times."""
        deadline = time.monotonic() + timeout
        with self.lock:
            while sum(1 for line in self.lines if text in line) < times:
                left = deadline - time.monotonic()
                if left <= 0 or (self.lines and self.lines[-1] == ""):
                    return False
                self.lock.wait(left)
        return True

    def send(self, command):
        self.sent += 1
        tmp = self.control + ".tmp"
        with open(tmp, "w", encoding="ascii") as fh:
            fh.write(f"{self.sent} {command}\n")
        os.replace(tmp, self.control)

    def output(self):
        with self.lock:
            return "".join(self.lines)


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_translucent_activity_test.py <a4l> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    if not os.path.isfile(android_jar):
        return skip("no staged android.jar")
    if not glob.glob("/dev/dri/renderD*"):
        return skip("no render node")
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not runtime_dir or not os.path.isdir(runtime_dir):
        return skip("XDG_RUNTIME_DIR not set")
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
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
    work = tempfile.mkdtemp(prefix="a4l-translucent-")
    weston = app = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, appdir, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "translucent_activity/Main.java"),
             os.path.join(fixtures, "gl_surface_under_views/Main.java")])
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        package = "com.acl.translucent"
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(
                utf8=True, package=package, launcher=f"{package}.Main",
                activities=[(f"{package}.Main$Map", None),
                            (f"{package}.Main$Welcome", THEME_TRANSLUCENT_NO_TITLE_BAR),
                            (f"{package}.Main$Cover", None)]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "translucent.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)
        control = os.path.join(work, "control")
        env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="120", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", A4L_TEST_CONTROL=control,
                   XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        app = subprocess.Popen([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                "--verify=off", "--present"], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               errors="replace", start_new_session=True)
        fx = Fixture(app, control)

        def fail(step, why):
            sys.stderr.write(f"FAIL: {step}: {why}\n{fx.output()[-6000:]}\n")
            return 1

        def see(step, want):
            """The screen comes to show `want`: the frame after a change takes a moment."""
            wrong = ["not looked at"]
            for _ in range(20):
                wrong = mismatches(shot(display, work), want)
                if not wrong:
                    return None
                time.sleep(0.25)
            return fail(step, "; ".join(wrong))

        def log_is(step, want):
            """Each line of the fixture's log has been seen exactly this many times."""
            wrong = [f"'{text}' logged {fx.count(text)} times, not {n}"
                     for text, n in want.items() if fx.count(text) != n]
            return fail(step, "; ".join(wrong)) if wrong else None

        step = "Welcome over Map"
        if not fx.wait("TRANSLUCENT: welcome onResume", 1, 80):
            if app.poll() == 77:
                return skip("run-apk.sh (vendored framework not built)", fx.output()[-2000:])
            return fail(step, "Welcome did not start")
        if not fx.wait("GLPLANE: 120 frames in", 1):
            return fail(step, "the GL view under Welcome did not draw 120 frames")
        r = see(step, UNDER_WELCOME) or log_is(step, {
            "TRANSLUCENT: map onResume": 1, "TRANSLUCENT: map onPause": 1,
            "TRANSLUCENT: map onStop": 0, "GLPLANE: surfaceCreated": 1,
            "GLPLANE: surfaceDestroyed": 0})
        if r:
            return r

        step = "Welcome finished"
        fx.send("close")
        if not fx.wait("TRANSLUCENT: map onResume", 2):
            return fail(step, "Map was not resumed")
        r = see(step, MAP_ALONE) or log_is(step, {
            "TRANSLUCENT: welcome onDestroy": 1, "TRANSLUCENT: map onRestart": 0,
            "TRANSLUCENT: map onStart": 1, "TRANSLUCENT: map onStop": 0,
            "GLPLANE: surfaceCreated": 1, "GLPLANE: surfaceDestroyed": 0})
        if r:
            return r

        step = "Welcome again, dimmed"
        fx.send("open")
        if not fx.wait("TRANSLUCENT: welcome onResume", 2):
            return fail(step, "Welcome did not start again")
        r = see(step, UNDER_DIMMED_WELCOME) or log_is(step, {
            "TRANSLUCENT: map onPause": 2, "TRANSLUCENT: map onStop": 0,
            "GLPLANE: surfaceCreated": 1, "GLPLANE: surfaceDestroyed": 0})
        if r:
            return r

        step = "an opaque Activity over Welcome"
        fx.send("cover")
        if not fx.wait("TRANSLUCENT: cover onResume", 1):
            return fail(step, "Cover did not start")
        # The first Welcome was stopped when it finished: this is the second one's.
        r = see(step, COVERED) or log_is(step, {
            "TRANSLUCENT: welcome onStop": 2, "TRANSLUCENT: map onStop": 1,
            "TRANSLUCENT: map onPause": 2, "GLPLANE: surfaceDestroyed": 1})
        if r:
            return r

        step = "back to Welcome over Map"
        fx.send("uncover")
        if not fx.wait("TRANSLUCENT: welcome onResume", 3):
            return fail(step, "Welcome was not resumed")
        if not fx.wait("GLPLANE: 120 frames in", 2):
            return fail(step, "the GL view did not draw on its new surface")
        r = see(step, UNDER_DIMMED_WELCOME) or log_is(step, {
            "TRANSLUCENT: welcome onRestart": 1, "TRANSLUCENT: map onRestart": 1,
            "TRANSLUCENT: map onStart": 2, "TRANSLUCENT: map onResume": 2,
            "GLPLANE: surfaceCreated": 2, "GLPLANE: surfaceDestroyed": 1})
        if r:
            return r
        failed = [line.strip() for line in fx.output().splitlines() if "GLPLANE: FAILED" in line]
        if failed:
            return fail(step, failed[0])
        sys.stdout.write("OK: the Activity under a translucent one stayed on screen, paused, "
                         "with its GL surface, through five steps\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
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


if __name__ == "__main__":
    raise SystemExit(main())

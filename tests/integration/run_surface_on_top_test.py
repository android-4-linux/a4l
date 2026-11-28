# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/surface_on_top/Main.java; the Weston
# helpers of run_gl_surfaceview_test.py. No GPL-3.0 sources.
#
# A screen an engine draws with a clear background, over the screen before it: a Flutter
# screen in a translucent Activity. Its SurfaceView asks to be over its window
# (setZOrderOnTop) and is clear where the engine draws nothing. a4l showed every such
# view's picture on a plane under the app's views, through a hole, so what was clear in it
# was black: the screen underneath was gone.
#
# The fixture's Under is a red banner over blue. Card, a translucent Activity over it, is
# one such view, which draws a yellow rectangle in the middle and a strip of half black
# down the right edge. The test starts a headless Weston with its kiosk shell (so the
# window is the screen), runs the fixture with --present, and reads a screenshot: the
# banner and the blue around the rectangle, and both at half brightness under the strip.
#
# A second run (A4L_TEST_OPAQUE=1) has the view ask for no format with alpha. Its surface
# is then opaque, as on Android: black where it drew nothing.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, Weston with its kiosk shell and
# weston-screenshooter, and a render node. Exits 77 (skip) where any of these is missing.
#
# Usage: run_surface_on_top_test.py <a4l> <android.jar>
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

RED, BLUE, YELLOW = (0xC0, 0x20, 0x20), (0x20, 0x20, 0xC0), (0xE0, 0xC0, 0x20)


def dimmed(color):
    return tuple(round(c * 0.5) for c in color)


# The rectangle is 400x200 in the middle, and the strip the rightmost 100 pixels.
BANNER_AT, LEFT_AT, UNDER_AT = (200, BANNER // 2), (100, H // 2), (W // 2, H - 60)
CARD_AT, STRIP_BANNER_AT, STRIP_AT = (W // 2, H // 2), (W - 50, BANNER // 2), (W - 50, H // 2)
CLEAR = {
    "banner": (BANNER_AT, RED),
    "the screen underneath, left of the rectangle": (LEFT_AT, BLUE),
    "the screen underneath, under the rectangle": (UNDER_AT, BLUE),
    "rectangle": (CARD_AT, YELLOW),
    "strip over the banner": (STRIP_BANNER_AT, dimmed(RED)),
    "strip over the screen underneath": (STRIP_AT, dimmed(BLUE)),
}
BLACK = (0, 0, 0)
OPAQUE = {
    "where the banner is": (BANNER_AT, BLACK),
    "left of the rectangle": (LEFT_AT, BLACK),
    "under the rectangle": (UNDER_AT, BLACK),
    "rectangle": (CARD_AT, YELLOW),
    "strip": (STRIP_AT, BLACK),
}


def mismatches(image, want_all):
    if image is None:
        return ["no screenshot"]
    w, h, px = image
    if (w, h) != (W, H):
        return [f"the screen is {w}x{h}, not {W}x{H}"]
    wrong = []
    for name, ((x, y), want) in want_all.items():
        got = tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
        if any(abs(g - c) > TOLERANCE for g, c in zip(got, want)):
            wrong.append(f"{name} at {x},{y} is {got}, not {want}")
    return wrong


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_surface_on_top_test.py <a4l> <android.jar>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-ontop-")
    weston = app = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, appdir, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "surface_on_top/Main.java")])
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(
                utf8=True, package="com.acl.ontop", launcher="com.acl.ontop.Main",
                activities=[("com.acl.ontop.Main$Under", None),
                            ("com.acl.ontop.Main$Card", THEME_TRANSLUCENT_NO_TITLE_BAR)]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "ontop.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)
        for tag, opaque, want in (("clear", "0", CLEAR), ("opaque", "1", OPAQUE)):
            env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                       ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, tag, "cache"),
                       XDG_DATA_HOME=os.path.join(work, tag, "data"), A4L_TEST_OPAQUE=opaque)
            app = subprocess.Popen([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                    "--verify=off", "--present"], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   errors="replace", start_new_session=True)
            lines = []
            drawn = threading.Event()

            def read(app=app, lines=lines, drawn=drawn):
                for line in app.stdout:
                    lines.append(line)
                    if "ONTOP: 30 frames drawn" in line or "ONTOP: FAILED" in line:
                        drawn.set()
                drawn.set()

            threading.Thread(target=read, daemon=True).start()
            drawn.wait(50)
            out = "".join(lines)
            if app.poll() == 77:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            if "ONTOP: 30 frames drawn" not in out:
                sys.stderr.write(f"FAIL ({tag}): the engine's view did not draw\n"
                                 f"{out[-6000:]}\n")
                return 1
            wrong = ["no screenshot"]
            deadline = time.monotonic() + 10
            while wrong and time.monotonic() < deadline:
                time.sleep(0.5)
                wrong = mismatches(shot(display, work), want)
            if wrong:
                out = "".join(lines)
                sys.stderr.write(f"FAIL ({tag}): " + "; ".join(wrong) + f"\n{out[-4000:]}\n")
                return 1
            os.killpg(app.pid, 15)
            app.wait(timeout=10)
        sys.stdout.write("OK: the screen underneath shows where the view over it is clear, "
                         "and at half brightness under its half-black strip; a view with "
                         "no alpha in its format is opaque\n")
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

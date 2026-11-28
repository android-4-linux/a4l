# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/compositing_layer/Main.java; the
# Weston helpers of run_gl_surfaceview_test.py. No GPL-3.0 sources.
#
# A view drawn into a compositing layer of its own: View.setLayerType(LAYER_TYPE_HARDWARE)
# and RenderNode.setUseCompositingLayer, which is what Compose's
# CompositingStrategy.Offscreen is. a4l kept the layer type and made no layer, so what such
# a view erased with DST_OUT or DST_IN (a fading edge) was erased from the window under it:
# a dark band where the text of a scrolling title fades.
#
# The fixture lays five such views over a blue window. The test starts a headless Weston
# with its kiosk shell (so the window is the screen), runs the fixture with --present, and
# reads a screenshot:
#   - a yellow view with its right half erased: yellow, then the window's blue;
#   - a red view whose layer has a paint of half alpha: red and blue in equal parts;
#   - a red view whose layer's paint has the blend mode ADD: red plus blue;
#   - a red view whose layer's paint has a color filter: the filter's green;
#   - a RenderNode that is a layer and does not clip, with yellow drawn wider than itself
#     and its right half erased: blue outside the node on both sides, yellow, then blue.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, Weston with its kiosk shell and
# weston-screenshooter, and a render node. Exits 77 (skip) where any of these is missing.
#
# Usage: run_compositing_layer_test.py <a4l> <android.jar>
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

BLUE, YELLOW, RED = (0x20, 0x20, 0xC0), (0xE0, 0xC0, 0x20), (0xC0, 0x20, 0x20)
GREEN = (0x20, 0xC0, 0x20)
# Red at 128/255 over the blue.
HALF = tuple(round(r * 128 / 255 + b * 127 / 255) for r, b in zip(RED, BLUE))
SUM = tuple(min(255, r + b) for r, b in zip(RED, BLUE))
ROW, SECOND_ROW = 140, 360
WANT = {
    "the window": ((20, ROW), BLUE),
    "erased view, left half": ((90, ROW), YELLOW),
    "erased view, right half": ((190, ROW), BLUE),
    "view with a layer paint of half alpha": ((380, ROW), HALF),
    "left of the node": ((500, ROW), BLUE),
    "node, left half": ((570, ROW), YELLOW),
    "node, right half": ((670, ROW), BLUE),
    "right of the node": ((740, ROW), BLUE),
    "view with a layer paint that adds": ((140, SECOND_ROW), SUM),
    "view with a layer paint that has a color filter": ((380, SECOND_ROW), GREEN),
}


def mismatches(image):
    if image is None:
        return ["no screenshot"]
    w, h, px = image
    if (w, h) != (W, H):
        return [f"the screen is {w}x{h}, not {W}x{H}"]
    wrong = []
    for name, ((x, y), want) in WANT.items():
        got = tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
        if any(abs(g - c) > TOLERANCE for g, c in zip(got, want)):
            wrong.append(f"{name} at {x},{y} is {got}, not {want}")
    return wrong


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_compositing_layer_test.py <a4l> <android.jar>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-layer-")
    weston = app = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, appdir, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "compositing_layer/Main.java")])
        run([d8, "--min-api", "29", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.layer",
                                launcher="com.acl.layer.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "layer.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)
        env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        app = subprocess.Popen([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                "--verify=off", "--present"], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               errors="replace", start_new_session=True)
        lines = []
        drawn = threading.Event()

        def read():
            for line in app.stdout:
                lines.append(line)
                if "LAYER: drawn" in line or "LAYER: FAILED" in line:
                    drawn.set()
            drawn.set()

        threading.Thread(target=read, daemon=True).start()
        drawn.wait(50)
        out = "".join(lines)
        if app.poll() == 77:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        if "LAYER: drawn" not in out:
            sys.stderr.write(f"FAIL: the fixture did not draw\n{out[-6000:]}\n")
            return 1
        # The frame the views were drawn in reaches the screen a little after the log line.
        wrong = ["no screenshot"]
        deadline = time.monotonic() + 10
        while wrong and time.monotonic() < deadline:
            time.sleep(0.5)
            wrong = mismatches(shot(display, work))
        if wrong:
            out = "".join(lines)
            sys.stderr.write("FAIL: " + "; ".join(wrong) + f"\n{out[-4000:]}\n")
            return 1
        sys.stdout.write("OK: five views in layers of their own: what they erased was "
                         "their own, and their layers' paints were applied\n")
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

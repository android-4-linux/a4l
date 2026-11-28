# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/gl_surface_under_views/Main.java;
# the PNG reader of run_video_plane_test.py. No GPL-3.0 sources.
#
# A SurfaceView that an engine draws GL into, under the app's other views, in the app's
# second Activity (a map under a search box). a4l used to give such a view no surface: the
# per-frame check looked at the first Activity's views, and a view found there would have
# been treated as a video.
#
# The fixture draws green and blue halves with a light band along the bottom through EGL10
# on a thread of its own, under a red banner view. The test starts a headless Weston with
# its kiosk shell (so the window is the screen), runs the fixture with --present, and reads
# a screenshot: the banner over the GL picture, the picture the right way up and the right
# way round, and the render thread held to about a swap a refresh.
#
# A second run (A4L_TEST_LAYER=1) has the fixture draw its views into a compositing layer
# (View.setLayerType(LAYER_TYPE_HARDWARE)). The hole the GL view punches is then punched in
# the layer, and the picture has to show all the same, under the banner: a4l punches the
# hole in the window before it composites the layer.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, Weston with its kiosk shell and
# weston-screenshooter, and a render node. Exits 77 (skip) where any of these is missing.
#
# Usage: run_gl_surfaceview_test.py <a4l> <android.jar>
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from run_video_plane_test import read_png, skip, weston_tools

W, H, BANNER = 800, 480, 96
TOLERANCE = 12
WANT = {
    "banner": ((W // 2, BANNER // 2), (0xC0, 0x20, 0x20)),
    "left": ((W // 4, 200), (0x20, 0xC0, 0x20)),
    "right": ((3 * W // 4, 200), (0x20, 0x20, 0xC0)),
    "bottom left": ((W // 4, H - 40), (0xE0, 0xE0, 0xE0)),
    "bottom right": ((3 * W // 4, H - 40), (0xE0, 0xE0, 0xE0)),
}


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def start_weston(runtime_dir, work):
    """A headless Weston of W x H whose one window fills the screen, and that allows
    screenshots. Returns (process, socket name) or (None, reason)."""
    tools = weston_tools()
    if tools is None:
        return None, "weston with weston-screenshooter is not installed"
    name = f"a4l-glplane-test-{os.getpid()}"
    env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
    with open(os.path.join(work, "weston.log"), "wb") as log:
        proc = subprocess.Popen(
            [tools[0], "--backend=headless", "--renderer=gl", "--shell=kiosk",
             f"--width={W}", f"--height={H}", f"--socket={name}", "--idle-time=0", "--debug"],
            env=env, stdout=log, stderr=log, start_new_session=True)
    for _ in range(50):
        if os.path.exists(os.path.join(runtime_dir, name)):
            return proc, name
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    proc.kill()
    return None, "no headless Weston with a kiosk shell could be started"


def shot(display, work):
    for old in glob.glob(os.path.join(work, "wayland-screenshot-*.png")):
        os.unlink(old)
    r = subprocess.run([weston_tools()[1]], cwd=work,
                       env=dict(os.environ, WAYLAND_DISPLAY=display), capture_output=True)
    made = glob.glob(os.path.join(work, "wayland-screenshot-*.png"))
    return read_png(made[0]) if r.returncode == 0 and made else None


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_gl_surfaceview_test.py <a4l> <android.jar>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-glplane-")
    weston = app = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, appdir, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "gl_surface_under_views/Main.java")])
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.glplane",
                                launcher="com.acl.glplane.Main",
                                activities=[("com.acl.glplane.Main$Map", None)]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "glplane.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)
        for tag, layer, what in (("plain", "0", "views drawn as they are"),
                                 ("layer", "1", "views in a compositing layer")):
            env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="90", ACL_BIN=acl,
                       ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, tag, "cache"),
                       XDG_DATA_HOME=os.path.join(work, tag, "data"), A4L_TEST_LAYER=layer)
            app = subprocess.Popen([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                    "--verify=off", "--present"], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   errors="replace", start_new_session=True)
            lines = []
            paced = threading.Event()

            def read(app=app, lines=lines, paced=paced):
                for line in app.stdout:
                    lines.append(line)
                    if "GLPLANE: 120 frames in" in line or "GLPLANE: FAILED" in line:
                        paced.set()
                paced.set()

            threading.Thread(target=read, daemon=True).start()
            paced.wait(80)
            out = "".join(lines)
            if app.poll() == 77:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            failed = re.search(r"GLPLANE: (FAILED .*)", out)
            m = re.search(r"GLPLANE: 120 frames in (\d+) ms", out)
            if failed or not m:
                why = failed.group(1) if failed else "the fixture did not draw 120 frames"
                sys.stderr.write(f"FAIL ({what}): {why}\n{out[-6000:]}\n")
                return 1
            image = shot(display, work)
            if image is None:
                sys.stderr.write(f"FAIL ({what}): no screenshot\n")
                return 1
            w, h, px = image
            if (w, h) != (W, H):
                sys.stderr.write(f"FAIL ({what}): the screen is {w}x{h}, not {W}x{H}\n")
                return 1
            wrong = []
            for name, ((x, y), want) in WANT.items():
                got = tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
                if any(abs(g - c) > TOLERANCE for g, c in zip(got, want)):
                    wrong.append(f"{name} at {x},{y} is {got}, not {want}")
            if wrong:
                sys.stderr.write(f"FAIL ({what}): " + "; ".join(wrong) + f"\n{out[-4000:]}\n")
                return 1
            elapsed = int(m.group(1))
            # 120 swaps at 60 Hz are two seconds. Far less means nothing held the thread.
            if elapsed < 1400:
                sys.stderr.write(f"FAIL ({what}): 120 swaps took {elapsed} ms: the swap "
                                 "does not wait\n")
                return 1
            sys.stdout.write(f"OK ({what}): the GL picture under the banner, the right way "
                             f"up, 120 swaps in {elapsed} ms\n")
            os.killpg(app.pid, 15)
            app.wait(timeout=10)
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

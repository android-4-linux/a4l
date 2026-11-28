# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/path_approximate/Main.java; the
# Weston helpers of run_gl_surfaceview_test.py. No GPL-3.0 sources.
#
# Path.approximate, and the PathInterpolator Android builds on it. a4l walked the first
# contour at even steps of its length, a few hundred points to a curve. An easing curve that
# is level in x at an end, as (1, 0, 1, 1) is, came back with points a hair off the curve
# that step back in x, and PathInterpolator threw "The Path cannot loop back on itself":
# Spotify 9 makes one in a static initializer its main screen's state goes through, and drew
# nothing after signing in. A second contour was left out, and a path of one point came back
# as the origin.
#
# The fixture logs what approximate gives for a line, two contours, a closed box, one point
# and no path, each known exactly; checks a quarter circle's points against the circle; and
# makes the 121 interpolators whose control points' x are on a tenth from 0 to 1.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, Weston with its kiosk shell,
# and a render node. Exits 77 (skip) where any of these is missing.
#
# Usage: run_path_approximate_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from run_gl_surfaceview_test import first, start_weston
from run_video_plane_test import skip

PACKAGE = "com.acl.pathapprox"
WANT = (
    "PATH: line 0.0:0.0,0.0 1.0:10.0,0.0",
    "PATH: contours 0.0:0.0,0.0 0.5:10.0,0.0 0.5:20.0,5.0 1.0:20.0,15.0",
    "PATH: closed 0.0:0.0,0.0 0.25:10.0,0.0 0.5:10.0,10.0 0.75:0.0,10.0 1.0:0.0,0.0",
    "PATH: point 0.0:3.0,4.0 1.0:3.0,4.0",
    "PATH: empty 0.0:0.0,0.0 1.0:0.0,0.0",
    "PATH: arc on circle=true chords within=true fractions rise=true ends=true few=true",
    "PATH: interpolators made=121/121 close=true",
    "PATH: done",
)


def story_of(out):
    return "\n".join(ln[ln.index("PATH: "):][:300] for ln in out.splitlines() if "PATH: " in ln)


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_path_approximate_test.py <a4l> <android.jar>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-path-")
    weston = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, appdir, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "path_approximate/Main.java")])
        run([d8, "--min-api", "33", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "path.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)
        env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off",
                            "--present"], env=env, capture_output=True, text=True,
                           errors="replace", timeout=120)
        out = p.stdout + p.stderr
        if p.returncode == 77:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        got = story_of(out).splitlines()
        missing = [w for w in WANT if w not in got]
        if missing:
            sys.stderr.write("FAIL: missing\n  " + "\n  ".join(missing) + "\ngot\n"
                             f"{story_of(out)}\n{out[-2000:] if not got else ''}\n")
            return 1
        sys.stdout.write(f"{story_of(out)}\n")
        sys.stdout.write("OK: Path.approximate gives a path's own points, through every "
                         "contour, and the easing curves that are level at an end make "
                         "interpolators\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired as e:
        sys.stderr.write(f"FAIL: the app did not end: {e}\n")
        return 1
    finally:
        if weston is not None and weston.poll() is None:
            try:
                os.killpg(weston.pid, 15)
                weston.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                weston.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

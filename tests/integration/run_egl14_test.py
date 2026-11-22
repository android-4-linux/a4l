# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/egl14_offscreen/Main.java. No GPL-3.0
# sources.
#
# Offscreen GL through android.opengl.EGL14 on the default display, as a player's placeholder
# surface sets it up. Mesa's default display is its build's default platform, which fails to
# initialize where that platform's server is not running; a4l then gives EGL14 Mesa's
# surfaceless platform. Two runs, each must clear a pbuffer and read the pixel back:
#   - a display server named, but EGL_PLATFORM=x11 and no X server (a board that runs only a
#     Wayland compositor under a Mesa that defaults to X11);
#   - no display server in the environment at all (a --split app process).
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, Mesa's libEGL and a render node.
# Exits 77 (skip) where any of these is missing.
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

SKIP = 77


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_egl14_test.py <a4l> <android.jar>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-egl14-")
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls = os.path.join(work, "cls")
        app = os.path.join(work, "app")
        dexdir = os.path.join(work, "dex")
        for d in (cls, app, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "egl14_offscreen/Main.java")])
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.egl14",
                                launcher="com.acl.egl14.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "egl14.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        base = {k: v for k, v in os.environ.items()
                if k not in ("DISPLAY", "WAYLAND_DISPLAY", "EGL_PLATFORM")}
        cases = [("server named, X11 default", {"WAYLAND_DISPLAY": "a4l-egl14-test-none",
                                                "EGL_PLATFORM": "x11"}),
                 ("no display server", {})]
        notes = []
        for label, vary in cases:
            env = dict(base, **vary, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                       XDG_CACHE_HOME=os.path.join(work, "cache"),
                       XDG_DATA_HOME=os.path.join(work, "data"))
            p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                "--verify=off"],
                               env=env, capture_output=True, text=True, timeout=180)
            out = p.stdout + p.stderr
            if p.returncode == SKIP:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            if "EGL14 natives registered" not in out:
                return skip("this build has no EGL14 natives", out[-2000:])
            m = re.search(r"EGL14: (OK .*|FAILED .*)", out)
            if not m:
                sys.stderr.write(f"FAIL ({label}): the fixture did not report\n{out[-6000:]}\n")
                return 1
            if not m.group(1).startswith("OK") or not m.group(1).endswith("0,255,0"):
                sys.stderr.write(f"FAIL ({label}): {m.group(1)}\n{out[-6000:]}\n")
                return 1
            notes.append(f"{label}: {m.group(1)[3:]}")
        sys.stdout.write("OK: " + "; ".join(notes) + "\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: timed out\n")
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

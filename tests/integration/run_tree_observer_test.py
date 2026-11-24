# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/tree_observer/Main.java. No GPL-3.0
# sources.
#
# A view's ViewTreeObserver listeners. A fixture app's launcher Activity starts the real one from
# its onCreate and finishes; the real one's view registers an OnGlobalLayoutListener before it
# is in a window, and the window's observer gets an OnPreDrawListener and an OnDrawListener. a4l
# runs measure, layout and draw from its own present loop, so it has to call these where
# ViewRootImpl does: global layout after a layout pass (the first, and one a view requests
# later), pre-draw before each draw (false cancels that frame), then draw. The fixture logs
# "TREE:" lines and "TREE: PASS" at the end.
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain, a render node and a Wayland
# compositor: the main looper runs under the GPU present path (--present, ACL_PRESENT_GPU). It
# starts a headless Weston on its own socket unless ACL_TEST_WAYLAND_DISPLAY names one. Exits 77
# (skip) where any of these is missing.
#
# Usage: run_tree_observer_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77

PACKAGE = "com.acl.tree"

CHECKS = [
    "TREE: global layout sized draws=0",
    "TREE: pre-draw cancelled one frame: draws then=0",
    "TREE: global layout again after requestLayout",
    "TREE: draw listener called=true view drawn=true",
    "TREE: PASS",
]


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def start_weston(runtime_dir, log_path):
    """A headless Weston on a private socket. Returns (process, socket name) or (None, reason)."""
    name = f"a4l-tree-test-{os.getpid()}"
    for exe in ("/usr/bin/weston", shutil.which("weston")):
        if not exe or not os.access(exe, os.X_OK):
            continue
        env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                [exe, "--backend=headless", "--renderer=gl", f"--socket={name}",
                 "--idle-time=0"],
                env=env, stdout=log, stderr=log, start_new_session=True)
        for _ in range(50):
            if os.path.exists(os.path.join(runtime_dir, name)):
                return proc, name
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        proc.kill()
    return None, "no headless Weston could be started"


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_tree_observer_test.py <a4l> <android.jar>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-tree-")
    weston = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        app = os.path.join(work, "app")
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        for d in (app, cls, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "tree_observer/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "tree-observer.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1",
                   XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off",
                            "--present"], env=env, capture_output=True, text=True, timeout=240)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        for c in CHECKS:
            if c not in out:
                sys.stderr.write(f"FAIL: missing '{c}'\n{out[-8000:]}\n")
                return 1
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        if weston is not None:
            # SIGTERM, so Weston removes its socket and lock file.
            weston.terminate()
            try:
                weston.wait(timeout=10)
            except subprocess.TimeoutExpired:
                weston.kill()
                weston.wait()
        shutil.rmtree(work, ignore_errors=True)

    sys.stdout.write("OK: global layout, pre-draw and draw listeners were called, for a view "
                     "registered before its window\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

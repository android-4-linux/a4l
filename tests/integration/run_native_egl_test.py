# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/native_egl_under_views; the build
# recipe of run_anw_producer_test.py and the Weston helpers of run_gl_surfaceview_test.py.
# No GPL-3.0 sources.
#
# A screen drawn by an engine through native EGL, opened from an app's own views: what a
# Flutter screen in an app otherwise built of views is. a4l took any engine that asked for
# the default display for one that owns the whole window, and made a second GL window on
# the toplevel its view tree is drawn to.
#
# The fixture's library does what such an engine does, on a thread of its own: asks for an
# offscreen (pbuffer) config and makes a 1x1 pbuffer current, makes a window surface from
# the SurfaceView's ANativeWindow, destroys it and makes it again eight times, as an engine
# does when the view is given its size, and then draws green and blue halves with a light
# band along the bottom. The view is in the app's second Activity, under a red banner view.
# The test starts a headless Weston with its kiosk shell, runs the fixture with --present,
# and reads a screenshot: the banner over the picture, the right way up and round.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain and a built bionic, Weston with
# its kiosk shell and weston-screenshooter, and a render node. Exits 77 (skip) where any of
# these is missing.
#
# Usage: run_native_egl_test.py <a4l> <source_root> <android.jar>
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

from run_gl_surfaceview_test import H, TOLERANCE, W, WANT, first, shot, start_weston
from run_video_plane_test import skip


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_native_egl_test.py <a4l> <src_root> <android.jar>\n")
        return 2
    acl, src_root, android_jar = sys.argv[1:]
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
    clang = first(os.path.join(root, "prebuilts/clang/host/linux-x86/clang-r450784d/bin/clang"))
    zipalign = first(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("clang", clang), ("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            return skip(f"{name} not found")

    # The bionic the library links against, by full path, so that its sonames are bionic's.
    inter = os.path.join(root, "out/soong/.intermediates")
    libc = os.path.join(inter, "bionic/libc/libc/android_x86_64_shared/libc.so")
    liblog = os.path.join(inter, "system/logging/liblog/liblog/android_x86_64_shared/liblog.so")
    libdl = os.path.join(inter, "bionic/libdl/libdl/android_x86_64_shared/libdl.so")
    crtbegin = os.path.join(inter, "bionic/libc/crtbegin_so/android_x86_64/crtbegin_so.o")
    crtend = os.path.join(
        inter, "bionic/libc/crtend_so/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend_so.o")
    inc_jni = os.path.join(root, "libnativehelper/include_jni")
    inc_log = os.path.join(root, "system/logging/liblog/include")
    for f in (libc, liblog, libdl, crtbegin, crtend):
        if not os.path.exists(f):
            return skip(f"no built bionic ({os.path.basename(f)})")

    def run(cmd, **kw):
        subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

    fixtures = os.path.join(src_root, "tests/fixtures")
    tools = os.path.join(src_root, "tools")
    work = tempfile.mkdtemp(prefix="a4l-nativeegl-")
    weston = app = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        # a4l's own libandroid.so, libEGL.so and libGLESv2.so, for the library to link.
        stage = os.path.join(work, "bionic-runtime")
        try:
            run([os.path.join(tools, "stage-bionic-runtime.sh"), stage],
                env=dict(os.environ, AOSP=root))
        except subprocess.CalledProcessError as e:
            return skip("stage-bionic-runtime failed", e.stderr[-600:])
        staged = [os.path.join(stage, "bionic", n)
                  for n in ("libandroid.so", "libEGL.so", "libGLESv2.so")]
        for f in staged:
            if not os.path.isfile(f):
                return skip(f"staged {os.path.basename(f)} absent")
        so = os.path.join(work, "libnativeegl.so")
        run([clang, "--target=x86_64-linux-android31", "-fPIC", "-shared", "-nostdlib",
             "-fno-emulated-tls", "-O1", "-Wl,-soname,libnativeegl.so", "-I", inc_jni,
             "-I", inc_log, "-o", so, crtbegin,
             os.path.join(fixtures, "native_egl_under_views/native_egl.c"), crtend,
             libc, liblog, libdl] + staged + ["-fuse-ld=lld"])

        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, os.path.join(appdir, "lib/x86_64"), dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "native_egl_under_views/Main.java")])
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        run(["cp", so, os.path.join(appdir, "lib/x86_64/")])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.nativeegl",
                                launcher="com.acl.nativeegl.Main",
                                activities=[("com.acl.nativeegl.Main$Screen", None)]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "nativeegl.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run(["zip", "-X", "-0", "-q", unaligned, "lib/x86_64/libnativeegl.so"], cwd=appdir)
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)
        env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="90", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        app = subprocess.Popen([os.path.join(tools, "run-apk.sh"), apk,
                                "--verify=off", "--present"], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               errors="replace", start_new_session=True)
        lines = []
        drawn = threading.Event()

        def read():
            for line in app.stdout:
                lines.append(line)
                if "NATIVEEGL: 120 frames in" in line or "NATIVEEGL: FAILED" in line:
                    drawn.set()
            drawn.set()

        threading.Thread(target=read, daemon=True).start()
        drawn.wait(80)
        out = "".join(lines)
        if app.poll() == 77:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        failed = re.search(r"NATIVEEGL: (FAILED .*)", out)
        m = re.search(r"NATIVEEGL: 120 frames in (\d+) ms", out)
        if failed or not m:
            why = failed.group(1) if failed else "the engine did not draw 120 frames"
            sys.stderr.write(f"FAIL: {why}\n{out[-6000:]}\n")
            return 1
        for said in ("NATIVEEGL: pbuffer current", "NATIVEEGL: window surface made again"):
            if said not in out:
                sys.stderr.write(f"FAIL: missing '{said}'\n{out[-4000:]}\n")
                return 1
        image = shot(display, work)
        if image is None:
            sys.stderr.write("FAIL: no screenshot\n")
            return 1
        w, h, px = image
        if (w, h) != (W, H):
            sys.stderr.write(f"FAIL: the screen is {w}x{h}, not {W}x{H}\n")
            return 1
        wrong = []
        for name, ((x, y), want) in WANT.items():
            got = tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
            if any(abs(g - c) > TOLERANCE for g, c in zip(got, want)):
                wrong.append(f"{name} at {x},{y} is {got}, not {want}")
        if wrong:
            sys.stderr.write("FAIL: " + "; ".join(wrong) + f"\n{out[-4000:]}\n")
            return 1
        sys.stdout.write("OK: an engine's native EGL picture under the banner, after a pbuffer "
                         f"and nine window surfaces, 120 swaps in {m.group(1)} ms\n")
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

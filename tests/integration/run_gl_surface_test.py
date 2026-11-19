# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/gl_surface/Main.java. No GPL-3.0
# sources.
#
# A GLSurfaceView that is laid out again while it renders, single-process and across the
# --split boundary.
#
# A fixture app clears a GLSurfaceView to a color every frame and, 1.5 s in, changes the
# view's layout and asks for a layout pass. The test asserts, in both modes:
#   - the renderer went on drawing afterwards;
#   - its surface was created once and the app was never told it was destroyed;
#   - no framework native was found missing on the way.
# A video SurfaceView failed all three before a4l stopped marking it as having a frame
# (tests/fixtures/video_plane covers that); this holds the GL path to the same.
#
# With "resize" as a third argument it resizes the window instead, in both modes: a
# headless sway is started, the app's window is floated and set to 800x600 once the
# renderer is up, and the renderer must be told (onSurfaceChanged 800x600) and go on
# drawing. Across the boundary the broker must also get a frame of that size and refuse
# none. Weston's headless backend has no way to resize a client's window, so this part
# needs sway, and is a test of its own so that a machine without sway reports a skip for
# it: as one test, the layout part passing hid that the resize was not checked.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, bubblewrap and a Wayland
# compositor; it starts a headless Weston on its own socket unless ACL_TEST_WAYLAND_DISPLAY
# names one. Exits 77 (skip) where any of these is missing.
#
# ACL_TEST_GLSURFACE_KEEP=<dir> keeps the APK and the runs' logs there.
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77
MIN_FRAMES_AFTER = 30  # 2 s follow the layout; a stalled renderer draws none


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
    name = f"a4l-glsurface-test-{os.getpid()}"
    for exe in ("/usr/bin/weston", shutil.which("weston")):
        if not exe or not os.access(exe, os.X_OK):
            continue
        env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                [exe, "--backend=headless", "--renderer=gl", f"--socket={name}", "--idle-time=0"],
                env=env, stdout=log, stderr=log, start_new_session=True)
        for _ in range(50):
            if os.path.exists(os.path.join(runtime_dir, name)):
                return proc, name
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        proc.kill()
    return None, "no headless Weston could be started"


def listening(path):
    """Whether something accepts a connection on the socket at `path`."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
        return True
    except OSError:
        return False
    finally:
        s.close()


def start_sway(runtime_dir, work):
    """A headless sway with one 1280x800 output. Returns (process, wayland socket, ipc
    socket) or (None, reason, None)."""
    sway, swaymsg = shutil.which("sway"), shutil.which("swaymsg")
    if not sway or not swaymsg:
        return None, "sway is not installed", None
    conf = os.path.join(work, "sway.conf")
    with open(conf, "w") as fh:
        fh.write("output HEADLESS-1 resolution 1280x800\ndefault_border none\n")
    ipc = os.path.join(work, "sway.sock")
    # Sway's socket is the one that is listening now and was not before. A name alone
    # does not tell: a compositor that was killed leaves its socket file behind, and sway
    # takes the first name nobody holds the lock of, which is that one.
    before = {p for p in glob.glob(os.path.join(runtime_dir, "wayland-*"))
              if not p.endswith(".lock") and listening(p)}
    env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "DISPLAY")}
    env.update(WLR_BACKENDS="headless", WLR_LIBINPUT_NO_DEVICES="1", SWAYSOCK=ipc)
    with open(os.path.join(work, "sway.log"), "wb") as log:
        proc = subprocess.Popen([sway, "-c", conf], env=env, stdout=log, stderr=log,
                                start_new_session=True)
    for _ in range(50):
        new = [p for p in glob.glob(os.path.join(runtime_dir, "wayland-*"))
               if p not in before and not p.endswith(".lock") and listening(p)]
        if new and os.path.exists(ipc):
            return proc, os.path.basename(new[0]), ipc
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    proc.kill()
    return None, "no headless sway could be started", None


def main() -> int:
    resize_only = len(sys.argv) == 4 and sys.argv[3] == "resize"
    if len(sys.argv) != 3 and not resize_only:
        sys.stderr.write("usage: run_gl_surface_test.py <a4l> <android.jar> [resize]\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    if not android_jar or not os.path.isfile(android_jar):
        return skip("no staged android.jar")
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
    sys.path.insert(0, fixtures)
    from make_axml import build_axml  # noqa: E402

    keep = os.environ.get("ACL_TEST_GLSURFACE_KEEP", "")
    work = tempfile.mkdtemp(prefix="a4l-glsurface-")
    weston = None
    sway = None
    try:
        # 1. The fixture APK.
        app = os.path.join(work, "app")
        os.makedirs(app)
        cls = os.path.join(work, "cls")
        os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "gl_surface/Main.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.glsurface",
                                launcher="com.acl.glsurface.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "gl-surface.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        # 2. A compositor. The resize runs have sway, started below.
        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display and not resize_only:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        env = dict(os.environ)
        env.update(WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        runner = [os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off"]

        def launch(args, tag):
            p = subprocess.run(runner + args, env=env, capture_output=True, text=True,
                               timeout=180)
            out = p.stdout + p.stderr
            if keep:
                os.makedirs(keep, exist_ok=True)
                shutil.copy(apk, keep)
                with open(os.path.join(keep, f"run-{tag}.log"), "w") as fh:
                    fh.write(out)
            return p.returncode, out

        def check(tag, out):
            """What must hold in either mode. Returns (error, summary)."""
            if "-> GL present" not in out:
                return "the GLSurfaceView was not taken for a GL surface", None
            if "UnsatisfiedLinkError" in out:
                return "a framework native is missing", None
            if "GLV: surfaceDestroyed" in out:
                return "the app was told its surface was destroyed", None
            if "GLV: laid out again" not in out:
                return "the fixture did not lay its view out again", None
            m = re.search(r"GLV: frames (\d+) after (\d+) created (\d+) changed (\d+) "
                          r"destroyed (\d+)", out)
            if not m:
                return "the fixture did not finish", None
            frames, after, created, _, destroyed = (int(v) for v in m.groups())
            if created != 1 or destroyed != 0:
                return f"the surface was created {created} time(s), destroyed {destroyed}", None
            if after < MIN_FRAMES_AFTER:
                return f"the renderer drew {after} frame(s) after the layout", None
            return None, f"{tag}: {frames} frames, {after} after the layout"

        def fail(why, out):
            sys.stderr.write(f"FAIL: {why}\n{out[-6000:]}\n")
            return 1

        if not resize_only:
            # 3. Single process.
            rc, out = launch(["--present"], "single")
            if rc == SKIP:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            if "needs the GPU present path" in out or "CreateWayland failed" in out:
                return skip("no GL present path here", out[-2000:])
            err, single = check("single process", out)
            if err:
                return fail(f"single process: {err}", out)

            # 4. Across the boundary.
            rc, out = launch(["--split", "--present"], "split")
            if "confinement is not available" in out or "cannot confine" in out:
                return skip("the app process cannot be confined here", out[-2000:])
            if "needs the GPU present path" in out:
                sys.stdout.write(f"OK: {single}; --split not built (no GPU present path)\n")
                return 0
            err, split = check("--split", out)
            if err:
                return fail(f"--split: {err}", out)
            s = re.search(r"split: app process ended \((\w+)\); .*?(\d+) message\(s\) rejected", out)
            if not s:
                return fail("no broker summary", out)
            if s.group(1) != "exited":
                return fail(f"the app process {s.group(1)}", out)
            if int(s.group(2)) != 0:
                return fail(f"the broker rejected {s.group(2)} message(s)", out)
            sys.stdout.write(f"OK: {single}; {split}\n")
            return 0

        # 5. A resize under sway, in both modes.
        sway, sway_display, ipc = start_sway(runtime_dir, work)
        if sway is None:
            return skip(sway_display)
        renv = dict(env, WAYLAND_DISPLAY=sway_display)
        exit_codes = []  # of each resize run, in order

        def resize_run(args, tag):
            """Run the fixture and set its window to 800x600 once the renderer is up.
            Returns (error, summary, output)."""
            p = subprocess.Popen(runner + args, env=renv, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, errors="replace")
            lines, asked = [], False
            deadline = time.time() + 120
            for line in p.stdout:
                lines.append(line)
                if not asked and "GLV: onSurfaceChanged" in line:
                    asked = True

                    def resize():
                        # The renderer is up before the window's first commit maps it,
                        # and sway floats only a window it has: ask until it says it did.
                        msg = [shutil.which("swaymsg"), "-s", ipc]
                        for _ in range(40):
                            r = subprocess.run(msg + ["floating", "enable"],
                                               capture_output=True, text=True)
                            if '"success": true' in r.stdout:
                                break
                            time.sleep(0.05)
                        subprocess.run(msg + ["resize", "set", "800", "600"],
                                       capture_output=True)

                    threading.Thread(target=resize, daemon=True).start()
                if time.time() > deadline:
                    p.kill()
                    break
            exit_codes.append(p.wait(timeout=30))
            out = "".join(lines)
            if keep:
                os.makedirs(keep, exist_ok=True)
                shutil.copy(apk, keep)
                with open(os.path.join(keep, f"run-{tag}.log"), "w") as fh:
                    fh.write(out)
            if not asked:
                return "the renderer never came up under sway", None, out
            err, summary = check(tag, out)
            if err:
                return err, None, out
            if "GL session: resized to 800x600" not in out:
                return "a4l did not take up the compositor's 800x600", None, out
            if "GLV: onSurfaceChanged 800x600" not in out:
                return "the renderer was not told its surface is 800x600", None, out
            return None, summary, out

        err, resized, out = resize_run(["--present"], "resize")
        if exit_codes[-1] == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        if "needs the GPU present path" in out or "CreateWayland failed" in out:
            return skip("no GL present path here", out[-2000:])
        if err:
            return fail(f"resize: {err}", out)
        # Across the boundary the broker must also have had a frame of the new size from
        # the app process, and refused none on the way.
        err, resized_split, out = resize_run(["--split", "--present"], "resize --split")
        if "confinement is not available" in out or "cannot confine" in out:
            return skip("the app process cannot be confined here", out[-2000:])
        if err:
            return fail(f"resize --split: {err}", out)
        if "broker: the child took the resize to 800x600" not in out:
            return fail("resize --split: no 800x600 frame reached the broker", out)
        s = re.search(r"split: app process ended \((\w+)\); .*?(\d+) message\(s\) rejected", out)
        if not s or s.group(1) != "exited" or int(s.group(2)) != 0:
            return fail("resize --split: the app process did not exit cleanly, or the "
                        "broker rejected a message", out)
        sys.stdout.write(f"OK: {resized}; {resized_split}; renderer told 800x600 in both\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired as e:
        sys.stderr.write(f"FAIL: timed out\n{(e.stdout or '')[-4000:]}\n")
        return 1
    finally:
        if sway is not None:
            sway.terminate()
            try:
                sway.wait(timeout=5)
            except subprocess.TimeoutExpired:
                sway.kill()
        if weston is not None:
            weston.terminate()
            try:
                weston.wait(timeout=5)
            except subprocess.TimeoutExpired:
                weston.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

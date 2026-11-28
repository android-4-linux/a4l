# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/view_resize/Main.java. No GPL-3.0
# sources.
#
# A View-tree app whose window the compositor resizes, single-process and across the
# --split boundary.
#
# A fixture app fills its window with one color and a square of another in each corner. A
# headless sway is started, the app's window is floated and set to 800x600 once its view
# has a size, and the test asserts, in both modes:
#   - a4l took the size up, and the app's view was laid out at 800x600;
#   - the app was told as Android tells it: onConfigurationChanged in the application's
#     callbacks, the activity and the view, with resources that say 800x600;
#   - the activity was not created a second time;
#   - with grim installed, a screenshot has the corner color in the window's far corner and
#     the compositor's background just outside it, so the picture is the window's size.
# Across the boundary the broker must also get a frame of that size and refuse none.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, bubblewrap and sway (Weston's
# headless backend cannot resize a client's window). Exits 77 (skip) where any is missing.
#
# ACL_TEST_VIEWRESIZE_KEEP=<dir> keeps the APK, the runs' logs and the screenshots there.
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


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


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


def screenshot(display, path):
    """A PPM of the output, or None where grim is not installed or fails."""
    grim = shutil.which("grim")
    if not grim:
        return None
    env = dict(os.environ, WAYLAND_DISPLAY=display)
    r = subprocess.run([grim, "-t", "ppm", path], env=env, capture_output=True)
    if r.returncode != 0 or not os.path.isfile(path):
        return None
    with open(path, "rb") as fh:
        data = fh.read()
    # P6, width, height, maxval, then one whitespace byte and the pixels.
    parts = data.split(None, 4)
    if len(parts) < 5 or parts[0] != b"P6":
        return None
    w, h = int(parts[1]), int(parts[2])
    px = parts[4]
    if len(px) < w * h * 3:
        return None

    def at(x, y):
        o = (y * w + x) * 3
        return px[o], px[o + 1], px[o + 2]

    return at


def near(c, want):
    return all(abs(a - b) <= 24 for a, b in zip(c, want))


FILL, CORNER = (0x20, 0xA0, 0x40), (0xE0, 0x20, 0x20)


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_view_resize_test.py <a4l> <android.jar>\n")
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

    keep = os.environ.get("ACL_TEST_VIEWRESIZE_KEEP", "")
    work = tempfile.mkdtemp(prefix="a4l-viewresize-")
    sway = None
    try:
        # 1. The fixture APK.
        app = os.path.join(work, "app")
        os.makedirs(app)
        cls = os.path.join(work, "cls")
        os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "view_resize/Main.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.viewresize",
                                launcher="com.acl.viewresize.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "view-resize.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])
        if keep:
            os.makedirs(keep, exist_ok=True)
            shutil.copy(apk, keep)

        # 2. A compositor that can resize a window.
        sway, display, ipc = start_sway(runtime_dir, work)
        if sway is None:
            return skip(display)
        env = dict(os.environ)
        env.update(WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        runner = [os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off"]
        msg = [shutil.which("swaymsg"), "-s", ipc]

        def resize_run(args, tag):
            """Run the fixture and set its window to 800x600 once its view has a size.
            Returns (output, pixel reader or None)."""
            p = subprocess.Popen(runner + args, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, errors="replace")
            lines, state = [], {"asked": False, "at": None}

            def resize():
                # The view is laid out before the window's first commit maps it, and sway
                # floats only a window it has: ask until it says it did.
                for _ in range(40):
                    r = subprocess.run(msg + ["floating", "enable"], capture_output=True,
                                       text=True)
                    if '"success": true' in r.stdout:
                        break
                    time.sleep(0.05)
                subprocess.run(msg + ["resize", "set", "800", "600"], capture_output=True)
                subprocess.run(msg + ["move", "position", "0", "0"], capture_output=True)

            def shoot():
                time.sleep(1.0)  # the app exits 2.5 s after it is told
                shot = os.path.join(keep or work, f"shot-{tag}.ppm")
                state["at"] = screenshot(display, shot)

            deadline = time.time() + 120
            shooter = None
            for line in p.stdout:
                lines.append(line)
                if not state["asked"] and "VR: size" in line:
                    state["asked"] = True
                    threading.Thread(target=resize, daemon=True).start()
                if shooter is None and "VR: size 800x" in line:
                    shooter = threading.Thread(target=shoot, daemon=True)
                    shooter.start()
                if time.time() > deadline:
                    p.kill()
                    break
            p.wait(timeout=30)
            if shooter is not None:
                shooter.join(timeout=10)
            out = "".join(lines)
            if keep:
                with open(os.path.join(keep, f"run-{tag}.log"), "w") as fh:
                    fh.write(out)
            return out, state["at"]

        def check(out, at):
            """What must hold in either mode. Returns (error, summary)."""
            if "needs the GPU present path" in out or "CreateWayland failed" in out:
                return SKIP, "no GPU present path here"
            if "VR: size" not in out:
                return "the app's view never got a size", None
            if "UnsatisfiedLinkError" in out:
                return "a framework native is missing", None
            if "VR: package version 42 / 1.2.3" not in out:
                return "PackageInfo did not carry the manifest's version (42 / 1.2.3)", None
            if "View session: resized to 800x600" not in out:
                return "a4l did not take up the compositor's 800x600", None
            # The view is the window less the decor's title bar, which keeps its height.
            sizes = [(int(w), int(h)) for w, h in re.findall(r"VR: size (\d+)x(\d+)", out)]
            first_w, first_h = sizes[0]
            opened = re.search(r"VR: created resources \d+x\d+ metrics (\d+)x(\d+)", out)
            if not opened or int(opened.group(1)) != first_w:
                return "the resources' metrics were not the window's when it opened", None
            bar = int(opened.group(2)) - first_h
            if sizes[-1] != (800, 600 - bar):
                return (f"the app's view was laid out at {sizes[-1]}, not 800x{600 - bar}"), None
            for who in ("application", "activity", "view"):
                if f"VR: {who} onConfigurationChanged 800x600" not in out:
                    return f"the {who} was not told of an 800x600 screen", None
            if not re.search(r"VR: activity onConfigurationChanged 800x600 resources 800x600 "
                             r"metrics 800x600 orientation 2", out):
                return "the resources did not say 800x600 when the activity was told", None
            # ViewConfiguration's limit on a View's software layer: four bytes a pixel of
            # the display, which is now 800x600.
            if not re.search(r"VR: activity onConfigurationChanged 800x600 .* layer limit "
                             + str(4 * 800 * 600) + r"\b", out):
                return "the software layer limit did not follow the display to 800x600", None
            m = re.search(r"VR: done \((.*?)\) configs (\d+) sizes (\d+) draws (\d+) created (\d+)",
                          out)
            if not m:
                return "the fixture did not finish", None
            if m.group(5) != "1":
                return "the activity was created again", None
            pixels = "no screenshot (grim not installed)"
            if at is not None:
                inside, outside, middle = at(790, 590), at(830, 300), at(400, 300)
                if not near(inside, CORNER):
                    return f"the window's far corner is {inside}, not the corner color", None
                if not near(middle, FILL):
                    return f"the window's middle is {middle}, not the fill color", None
                if near(outside, FILL) or near(outside, CORNER):
                    return f"the app's picture reaches past the window ({outside} at x=830)", None
                pixels = "picture is 800x600"
            return None, f"{m.group(2)} configuration change(s), {pixels}"

        def fail(why, out):
            sys.stderr.write(f"FAIL: {why}\n{out[-6000:]}\n")
            return 1

        out, at = resize_run(["--present"], "single")
        err, single = check(out, at)
        if err == SKIP:
            return skip(single, out[-2000:])
        if err:
            return fail(f"single process: {err}", out)

        out, at = resize_run(["--split", "--present"], "split")
        if "confinement is not available" in out or "cannot confine" in out:
            return skip("the app process cannot be confined here", out[-2000:])
        err, split = check(out, at)
        if err == SKIP:
            sys.stdout.write(f"OK: single process: {single}; --split not built\n")
            return 0
        if err:
            return fail(f"--split: {err}", out)
        if "broker: the child took the resize to 800x600" not in out:
            return fail("--split: no 800x600 frame reached the broker", out)
        s = re.search(r"split: app process ended \((\w+)\); .*?(\d+) message\(s\) rejected", out)
        if not s or s.group(1) != "exited" or int(s.group(2)) != 0:
            return fail("--split: the app process did not exit cleanly, or the broker "
                        "rejected a message", out)

        # 3. The first size. With a gap around it the tile is not the output's
        # 1280x800 that a4l opens at, and nothing resizes the window afterward.
        subprocess.run(msg + ["gaps", "outer", "all", "set", "40"], capture_output=True)
        for args, mode in ((["--present"], "single process"),
                           (["--split", "--present"], "--split")):
            p = subprocess.run(runner + args, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors="replace",
                               timeout=120)
            if keep:
                with open(os.path.join(keep, f"run-tile-{mode.strip('-').split()[0]}.log"),
                          "w") as fh:
                    fh.write(p.stdout)
            if "View session: resized to 1200x720" not in p.stdout:
                return fail(f"{mode}: a4l did not take up the 1200x720 tile it was "
                            "first given", p.stdout)
            if "VR: activity onConfigurationChanged 1200x720 resources 1200x720" not in p.stdout:
                return fail(f"{mode}: the activity was not told of the tile's size", p.stdout)
            if "rejected" in p.stdout and not re.search(r" 0 message\(s\) rejected", p.stdout):
                return fail(f"{mode}: the broker rejected a message", p.stdout)

        # 4. A compositor that lists LINEAR alone, as an embedded shell may. The frames
        # must be LINEAR, and under --split the broker must take them.
        lin = dict(env, ACL_PRESENT_LINEAR_ONLY="1")
        linear = "LINEAR-only frames taken in both"
        for args, mode in ((["--present"], "single process"),
                           (["--split", "--present"], "--split")):
            p = subprocess.run(runner + args, env=lin, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors="replace",
                               timeout=120)
            if "ACL_PRESENT_LINEAR_ONLY, LINEAR alone" not in p.stdout:
                linear = "LINEAR-only not run (the compositor does not list LINEAR)"
                break
            if not re.search(r"dmabuf present\(A\): \d+x\d+ linear modifier=0x0 ", p.stdout):
                return fail(f"{mode}: the frames were not LINEAR for a compositor that "
                            "lists LINEAR alone", p.stdout)
            if "refused a frame" in p.stdout:
                return fail(f"{mode}: the broker refused a LINEAR frame", p.stdout)
            if "VR: size" not in p.stdout:
                return fail(f"{mode}: the app's view never got a size with LINEAR frames",
                            p.stdout)
        sys.stdout.write(f"OK: single process: {single}; --split: {split}; "
                         f"a 1200x720 tile taken as the first size in both; {linear}\n")
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
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

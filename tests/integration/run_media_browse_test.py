# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/media_host/Main.java. No GPL-3.0 sources.
#
# `a4l run --service=<class> --present` gives a media browser service a window: the host
# layer's MediaBrowseActivity binds it, shows the root's browsable children as tabs and a
# tab's children as a list, plays a playable item through the session's transport controls
# and shows the session in a bar with the transport. The fixture's service has one tab,
# "Albums", whose first item is a track. The test taps that row and then the bar's
# play/pause button. The checks: the root and the tab are browsed, the tap asks the session
# to play the track, the session's callback is given it, and the second tap, with the session
# now playing, asks it to pause.
#
# The window is 1024x768 so the taps land: a 48-pixel tab row, 64-pixel list rows, and a
# 72-pixel bar at the bottom with three 56-pixel buttons at its right.
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain, a render node and Weston. Exits
# 77 (skip) where any of these is missing.
#
# Usage: run_media_browse_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

from run_notify_test import first, skip, stop

SKIP = 77
PACKAGE = "com.acl.mediahost"
SERVICE = f"{PACKAGE}.Main$Wrapped"
# In order.
WANT = [
    "acl media: browse root: root",
    "acl media: children of 'root': 2 item(s)",
    "acl media: children of 'albums': 2 item(s)",
    "acl media: play track-2",
    "MEDIAHOST: asked to play track-2",
    "MEDIAHOST: asked to pause",
]
# First list row; then the middle of the bar's three buttons.
TAPS = "300,80,6000;928,732,8000"


def start_weston(runtime_dir, log_path):
    """A headless 1024x768 Weston on a private socket. (process, socket name) or (None, why)."""
    name = f"a4l-browse-test-{os.getpid()}"
    for exe in ("/usr/bin/weston", shutil.which("weston")):
        if not exe or not os.access(exe, os.X_OK):
            continue
        env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                [exe, "--backend=headless", "--renderer=gl", f"--socket={name}",
                 "--idle-time=0", "--width=1024", "--height=768"],
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
        sys.stderr.write("usage: run_media_browse_test.py <a4l> <android.jar>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-media-browse-")
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
             os.path.join(fixtures, "media_host/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main",
                                services=[(SERVICE, ["android.media.browse.MediaBrowserService"])]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "mediahost.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
        if weston is None:
            return skip(display)
        env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="12", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", ACL_AUDIO="null", ACL_TEST_MS="1", ACL_TEST_TAP=TAPS,
                   XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off",
                            "--present", f"--service={SERVICE}"],
                           env=env, capture_output=True, text=True, timeout=200)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            return skip("tools/run-apk.sh skipped", out[-2000:])
        story = "\n".join(ln for ln in out.splitlines()
                          if "MEDIAHOST: " in ln or "acl media" in ln or "FATAL" in ln
                          or "pointer action=1" in ln)
        at = 0
        for want in WANT:
            at = out.find(want, at)
            if at < 0:
                sys.stderr.write(f"FAIL: missing, or out of order: {want}\n{story}\n")
                return 1
        if "FATAL EXCEPTION" in out:
            sys.stderr.write(f"FAIL: an uncaught exception\n{story}\n")
            return 1
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        stop(weston)
        shutil.rmtree(work, ignore_errors=True)

    print("PASS: the service was browsed in a window, a track was played from its row and "
          "paused from the bar")
    return 0


if __name__ == "__main__":
    sys.exit(main())

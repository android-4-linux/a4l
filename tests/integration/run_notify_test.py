# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/notify/Main.java. No GPL-3.0 sources.
#
# A notification reaches the host notification daemon with its picture and its actions, and a
# click on an action comes back. A fixture app posts a notification with a 600x300 large icon,
# a content intent and one action. The test runs a private session bus with a stand-in daemon
# (acl-notify-stub) on it, which prints each Notify call and invokes the action. The checks:
# the Notify carries the action and an image-data hint, the action's PendingIntent arrives in
# the app, an identical repost sends nothing, a changed one replaces the first, a cancel closes
# it, and a foreground Service's notification is shown and taken down by stopForeground.
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain, a render node, a Wayland
# compositor and dbus-daemon. It starts a headless Weston on its own socket unless
# ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip) where any of these is missing.
#
# Usage: run_notify_test.py <a4l> <android.jar> <acl-notify-stub>
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77

PACKAGE = "com.acl.notify"

# Lines the fixture logs, in order.
CHECKS = [
    "NOTIFY: launch intent com.acl.notify.Main",
    "NOTIFY: pending intent same=true other=true no create=true",
    "NOTIFY: posted",
    "NOTIFY: action received which=pause",
    "NOTIFY: PASS",
]
# What the stand-in daemon saw, in order. The image is width x height / rowstride / alpha /
# bits / channels / bytes / first pixel (RGBA). The app's large icon is 600x300;
# it reaches the desktop at 256 pixels on its long edge, not the 48dp NotificationManager
# would shrink it to for Android's shade.
SHOWN = "actions=default:,a0:Pause image=256x128/1024/1/8/4/131072/336699ff resident=1"
STUB = [
    f"STUB: notify id=1 replaces=0 app=Example summary=Track body=Artist {SHOWN}",
    f"STUB: notify id=1 replaces=1 app=Example summary=Next track \U0001f3b5 body=Artist {SHOWN}",
    "STUB: close id=1",
    f"STUB: notify id=2 replaces=0 app=Example summary=Foreground body=Artist {SHOWN}",
    "STUB: close id=2",
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
    name = f"a4l-notify-test-{os.getpid()}"
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


BUS_CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path={path}</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


def start_bus(work):
    """A private session bus. Returns (process, address) or (None, reason).

    Its configuration names no service directory, so the bus starts nothing on demand. With
    the session's default configuration a4l's start took 25 s longer here, the length of a
    D-Bus call's default timeout."""
    exe = shutil.which("dbus-daemon")
    if not exe:
        return None, "dbus-daemon not found"
    config = os.path.join(work, "bus.conf")
    with open(config, "w") as fh:
        fh.write(BUS_CONFIG.format(path=os.path.join(work, "bus")))
    proc = subprocess.Popen([exe, f"--config-file={config}", "--nofork", "--print-address=1"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                            start_new_session=True)
    address = proc.stdout.readline().strip()
    if not address:
        proc.kill()
        return None, "dbus-daemon did not start"
    return proc, address


def stop(proc):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main():
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_notify_test.py <a4l> <android.jar> <acl-notify-stub>\n")
        return 2
    acl, android_jar, stub_exe = sys.argv[1], sys.argv[2], sys.argv[3]
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
    work = tempfile.mkdtemp(prefix="a4l-notify-")
    weston = bus = stub = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        app = os.path.join(work, "app")
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        for d in (app, cls, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "notify/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main",
                                services=[(f"{PACKAGE}.Main$Playback", [])]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "notify.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        bus, address = start_bus(work)
        if bus is None:
            return skip(address)
        bus_env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
        stub_log = os.path.join(work, "stub.log")
        with open(stub_log, "wb") as log:
            stub = subprocess.Popen([stub_exe], env=bus_env, stdout=log,
                                    stderr=subprocess.STDOUT, start_new_session=True)
        for _ in range(50):
            with open(stub_log) as fh:
                if "STUB: ready" in fh.read():
                    break
            if stub.poll() is not None:
                with open(stub_log) as fh:
                    sys.stderr.write(f"FAIL: the stand-in daemon exited\n{fh.read()}\n")
                return 1
            time.sleep(0.1)

        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        env = dict(bus_env, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1",
                   XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off",
                            "--present"], env=env, capture_output=True, text=True, timeout=240)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        story = "\n".join(ln for ln in out.splitlines() if "NOTIFY: " in ln or "notify" in ln)
        pos = 0
        for c in CHECKS:
            at = out.find(c, pos)
            if at < 0:
                sys.stderr.write(f"FAIL: missing '{c}'\n{story}\n{out[-4000:]}\n")
                return 1
            pos = at + len(c)
        with open(stub_log) as fh:
            seen = [ln.strip() for ln in fh if ln.startswith("STUB: ") and "ready" not in ln]
        if seen != STUB:
            sys.stderr.write("FAIL: the daemon saw\n  " + "\n  ".join(seen)
                             + "\nexpected\n  " + "\n  ".join(STUB) + f"\n{story}\n")
            return 1
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        stop(stub)
        stop(bus)
        # SIGTERM, so Weston removes its socket and lock file.
        stop(weston)
        shutil.rmtree(work, ignore_errors=True)

    sys.stdout.write("OK: the notification reached the daemon with its picture and action, "
                     "and the action came back\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/mpris/Main.java; the MPRIS D-Bus
# Interface Specification 2.2 (freedesktop.org). No GPL-3.0 sources.
#
# An app's MediaSession is an MPRIS player on the session bus, and the desktop's commands
# reach the session. The fixture is a player with a session and nothing else. The test runs
# it on a private session bus and acts as the desktop with gdbus. The checks:
#   - the player takes org.mpris.MediaPlayer2.<package>, with the app's label as Identity
#     and the package as DesktopEntry
#   - PlaybackStatus, the title, artist, album and length, and what the session can do
#   - the artwork is a PNG file of the picture's size, for a Bitmap in the metadata and for
#     a content URI of the app's own provider
#   - Position moves while playing
#   - Pause, Play, Next and SetPosition arrive in the session's callback, and the player
#     shows what the session then did; a SetPosition for another track is ignored
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain, a render node, a Wayland
# compositor, dbus-daemon and gdbus. It starts a headless Weston on its own socket unless
# ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip) where any of these is missing.
#
# The desktop shows the app's playback once. The fixture posts its session's notification
# before the session has a track, so the stand-in notification daemon (acl-notify-stub) sees
# it; it is closed when the player comes onto the bus, the one posted with the second track
# is held back, a plain notification is shown throughout, and the held one is shown when
# Stop releases the session and the player leaves the bus.
#
# Usage: run_mpris_test.py <a4l> <android.jar> <acl-notify-stub>
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from run_notify_test import first, skip, start_bus, start_weston, stop

SKIP = 77
PACKAGE = "com.acl.mpris"
NAME = "org.mpris.MediaPlayer2.com_acl_mpris"
PATH = "/org/mpris/MediaPlayer2"
PLAYER = "org.mpris.MediaPlayer2.Player"


def main():
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_mpris_test.py <a4l> <android.jar> <acl-notify-stub>\n")
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
    gdbus = shutil.which("gdbus")
    if not gdbus:
        return skip("gdbus not found")
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
    work = tempfile.mkdtemp(prefix="a4l-mpris-")
    weston = bus = app = stub = None
    log_path = os.path.join(work, "app.log")
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        appdir = os.path.join(work, "app")
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        for d in (appdir, cls, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "mpris/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main",
                                providers=[(f"{PACKAGE}.Main$Art", "com.acl.mpris.art")]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "mpris.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
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
                sys.stderr.write("FAIL: the stand-in notification daemon exited\n")
                return 1
            time.sleep(0.1)
        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        env = dict(bus_env, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="90", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", ACL_AUDIO="null",
                   XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        with open(log_path, "wb") as log:
            app = subprocess.Popen([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                    "--verify=off", "--present"], env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)

        def app_log():
            with open(log_path, errors="replace") as fh:
                return fh.read()

        def fail(why):
            story = "\n".join(ln for ln in app_log().splitlines()
                              if "MPRIS: " in ln or "svc.mpris" in ln)
            sys.stderr.write(f"FAIL: {why}\n{story}\n")
            return 1

        def call(*args):
            p = subprocess.run([gdbus, "call", "--session", "-d", NAME, "-o", PATH, "-m",
                                *args], env=bus_env, capture_output=True, text=True,
                               timeout=20)
            return p.returncode, p.stdout + p.stderr

        def props(iface):
            rc, out = call("org.freedesktop.DBus.Properties.GetAll", iface)
            return out if rc == 0 else ""

        def until(what, test, seconds=30.0):
            deadline = time.monotonic() + seconds
            last = None
            while time.monotonic() < deadline:
                if app.poll() is not None:
                    if app.returncode == SKIP:
                        return None
                    break
                last = test()
                if last:
                    return last
                time.sleep(0.2)
            return False

        got = until("the player", lambda: props(PLAYER), 60.0)
        if got is None:
            return skip("run-apk.sh (vendored framework not built)", app_log()[-2000:])
        if not got:
            return fail("no player on the bus as " + NAME)
        for want in ("'PlaybackStatus': <'Playing'>", "'xesam:title': <'First'>",
                     "'xesam:artist': <['Band']>", "'xesam:album': <'Album'>",
                     "'mpris:length': <int64 200000000>", "'CanGoNext': <true>",
                     "'CanGoPrevious': <false>", "'CanSeek': <true>", "'CanPlay': <true>",
                     "'CanPause': <true>", "'CanControl': <true>"):
            if want not in got:
                return fail(f"the player's properties lack {want}:\n{got}")
        base = props("org.mpris.MediaPlayer2")
        for want in ("'Identity': <'Example'>", f"'DesktopEntry': <'{PACKAGE}'>"):
            if want not in base:
                return fail(f"the root's properties lack {want}:\n{base}")

        def artwork(text):
            """The size of the PNG the metadata's artUrl names, or None."""
            m = re.search(r"'mpris:artUrl': <'file://([^']+)'>", text)
            if not m or not os.path.isfile(m.group(1)):
                return None
            with open(m.group(1), "rb") as fh:
                head = fh.read(24)
            if head[:8] != b"\x89PNG\r\n\x1a\n":
                return None
            return struct.unpack(">II", head[16:24])
        if artwork(got) != (64, 32):
            return fail(f"the artwork is not a 64x32 PNG: {artwork(got)}\n{got}")

        def position():
            m = re.search(r"'Position': <int64 (\d+)>", props(PLAYER))
            return int(m.group(1)) if m else -1
        p1 = position()
        time.sleep(1.0)
        p2 = position()
        if not (5_000_000 <= p1 < p2 <= p1 + 5_000_000):
            return fail(f"Position went {p1} -> {p2} in a second of playing")

        rc, out = call(PLAYER + ".Pause")
        if rc != 0 or not until("paused", lambda: "<'Paused'>" in props(PLAYER)):
            return fail(f"Pause (exit {rc}) did not pause: {out}")
        rc, out = call(PLAYER + ".Play")
        if rc != 0 or not until("playing", lambda: "<'Playing'>" in props(PLAYER)):
            return fail(f"Play (exit {rc}) did not play: {out}")
        rc, out = call(PLAYER + ".Next")
        after = until("the next track", lambda: (lambda t: t if "<'Second'>" in t else "")(
            props(PLAYER)))
        if rc != 0 or not after:
            return fail(f"Next (exit {rc}) did not change the track: {out}")
        if "'mpris:length': <int64 100000000>" not in after:
            return fail(f"the next track's length is not 100 s:\n{after}")
        # The second track's picture comes from the app's provider, after the track does.
        if not until("the provider's picture", lambda: artwork(props(PLAYER)) == (40, 20)):
            return fail(f"the artwork is not the provider's 40x20 PNG:\n{props(PLAYER)}")
        track = re.search(r"'mpris:trackid': <objectpath '([^']+)'>", after)
        old = re.search(r"'mpris:trackid': <objectpath '([^']+)'>", got)
        if not track or not old or track.group(1) == old.group(1):
            return fail("the track id did not change with the track")
        # For the track that is gone: ignored. For the current one: a seek.
        call(PLAYER + ".SetPosition", old.group(1), "50000000")
        rc, out = call(PLAYER + ".SetPosition", track.group(1), "30000000")
        if rc != 0 or not until("the seek", lambda: "MPRIS: seek 30000" in app_log()):
            return fail(f"SetPosition (exit {rc}) did not seek: {out}")
        if "MPRIS: seek 50000" in app_log():
            return fail("a SetPosition for a track that is gone reached the session")
        if not until("the position", lambda: 29_000_000 <= position() <= 31_000_000):
            return fail(f"Position is {position()} after a seek to 30 s")

        # Stop releases the session: the player leaves the bus and the held notification
        # is shown.
        def owned():
            p = subprocess.run([gdbus, "call", "--session", "-d", "org.freedesktop.DBus", "-o",
                                "/org/freedesktop/DBus", "-m",
                                "org.freedesktop.DBus.NameHasOwner", NAME], env=bus_env,
                               capture_output=True, text=True, timeout=20)
            return "true" in p.stdout

        def stub_lines():
            with open(stub_log) as fh:
                return [re.sub(r" body=.*", "", ln.strip()) for ln in fh
                        if ln.startswith("STUB: ") and "ready" not in ln]
        shown = ["STUB: notify id=1 replaces=0 app=Example summary=Now playing",
                 "STUB: close id=1",
                 "STUB: notify id=2 replaces=0 app=Example summary=Plain"]
        # The close and the plain notification come from two threads, in either order.
        if sorted(stub_lines()) != sorted(shown):
            return fail("with the player on the bus the daemon saw\n  "
                        + "\n  ".join(stub_lines()) + "\nexpected\n  " + "\n  ".join(shown))
        rc, out = call(PLAYER + ".Stop")
        if rc != 0 or not until("the player gone", lambda: not owned()):
            return fail(f"Stop (exit {rc}) did not take the player off the bus: {out}")
        shown.append("STUB: notify id=3 replaces=0 app=Example summary=Now playing 2")
        if not until("the held notification",
                     lambda: sorted(stub_lines()) == sorted(shown)
                     and stub_lines()[-1] == shown[-1]):
            return fail("after the player left the daemon saw\n  "
                        + "\n  ".join(stub_lines()) + "\nexpected\n  " + "\n  ".join(shown))

        want = ["MPRIS: ready", "MPRIS: pause", "MPRIS: play", "MPRIS: next",
                "MPRIS: art read", "MPRIS: seek 30000", "MPRIS: stop"]
        seen = [m.group(0) for m in re.finditer(r"MPRIS: (art read|[a-z]+( \d+)?)", app_log())]
        if seen != want:
            return fail(f"the session was asked {seen}, not {want}")
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        if app is not None and app.poll() is None:
            try:
                os.killpg(app.pid, 15)
                app.wait(timeout=15)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(app.pid, 9)
                except OSError:
                    pass
        stop(stub)
        stop(bus)
        stop(weston)
        shutil.rmtree(work, ignore_errors=True)

    sys.stdout.write("OK: the session is an MPRIS player with its track, picture and "
                     "position, and the desktop's commands reached it\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

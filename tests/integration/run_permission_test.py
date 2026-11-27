# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/permission/Main.java. No GPL-3.0 sources.
#
# The microphone permission follows the desktop's permission store. A fixture app reads
# RECORD_AUDIO through checkSelfPermission, checkPermission, PackageManager.checkPermission
# and its PackageInfo, and asks for it with requestPermissions. The test runs a private
# session bus with a stand-in store (acl-permission-stub) on it, which has no entry for the
# app until the app creates a file and says yes after that. The checks: every reading is a
# denial before and a grant after, CAMERA is granted throughout, and the store was asked
# about the microphone for this package and nothing else.
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain, a render node, a Wayland
# compositor and dbus-daemon. It starts a headless Weston on its own socket unless
# ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip) where any of these is missing.
#
# Usage: run_permission_test.py <a4l> <android.jar> <acl-permission-stub>
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

from run_notify_test import first, skip, start_bus, start_weston, stop

SKIP = 77

PACKAGE = "com.acl.permission"

# Lines the fixture logs, in order. -1 is PERMISSION_DENIED, 0 PERMISSION_GRANTED.
CHECKS = [
    "PERM: before self -1 uid -1 pm -1 camera 0 declared RECORD_AUDIO=false CAMERA=true",
    "PERM: request 7 2 names mic -1 camera 0",
    "PERM: after self 0 uid 0 pm 0 camera 0 declared RECORD_AUDIO=true CAMERA=true",
    "PERM: request 8 2 names mic 0 camera 0",
    "PERM: PASS",
]
ASKED = f"STUB: get devices microphone {PACKAGE} -> "


def main():
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_permission_test.py <a4l> <android.jar> "
                         "<acl-permission-stub>\n")
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
    work = tempfile.mkdtemp(prefix="a4l-permission-")
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
             os.path.join(fixtures, "permission/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main",
                                permissions=["android.permission.RECORD_AUDIO",
                                             "android.permission.CAMERA"]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "permission.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        bus, address = start_bus(work)
        if bus is None:
            return skip(address)
        # ACL_ALLOW_DEVICES would grant the microphone whatever the store says.
        bus_env = {k: v for k, v in os.environ.items() if k != "ACL_ALLOW_DEVICES"}
        bus_env["DBUS_SESSION_BUS_ADDRESS"] = address
        data = os.path.join(work, "data")
        grant = os.path.join(data, "acl", PACKAGE, "files", "grant")
        stub_log = os.path.join(work, "stub.log")
        with open(stub_log, "wb") as log:
            stub = subprocess.Popen([stub_exe, grant], env=bus_env, stdout=log,
                                    stderr=subprocess.STDOUT, start_new_session=True)
        for _ in range(50):
            with open(stub_log) as fh:
                if "STUB: ready" in fh.read():
                    break
            if stub.poll() is not None:
                with open(stub_log) as fh:
                    sys.stderr.write(f"FAIL: the stand-in store exited\n{fh.read()}\n")
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
                   XDG_DATA_HOME=data)
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off",
                            "--present"], env=env, capture_output=True, text=True, timeout=240)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        story = "\n".join(ln for ln in out.splitlines()
                          if "PERM: " in ln or "permission" in ln.lower())
        pos = 0
        for c in CHECKS:
            at = out.find(c, pos)
            if at < 0:
                sys.stderr.write(f"FAIL: missing '{c}'\n{story}\n{out[-4000:]}\n")
                return 1
            pos = at + len(c)
        with open(stub_log) as fh:
            seen = [ln.strip() for ln in fh if ln.startswith("STUB: ") and "ready" not in ln]
        other = [ln for ln in seen if not ln.startswith(ASKED)]
        if other or ASKED + "unset" not in seen or ASKED + "yes" not in seen:
            sys.stderr.write("FAIL: the store saw\n  " + "\n  ".join(seen) + f"\n{story}\n")
            return 1
        # An answer is kept for a second, so the readings above are a few calls, not one each.
        if len(seen) > 6:
            sys.stderr.write(f"FAIL: the store was asked {len(seen)} times\n  "
                             + "\n  ".join(seen) + "\n")
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

    sys.stdout.write("OK: the microphone permission was denied until the store allowed it\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

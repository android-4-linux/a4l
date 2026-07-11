# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C compositor matrix),
# Addendum A02 (wcs windowing). No GPL-3.0 sources.
#
# Tier-C full-vertical test: run the real `acl run view.apk --present` against a
# headless Weston. This exercises the WHOLE stack — APK open, manifest, DEX->JVM,
# Activity lifecycle, the View measure/layout/draw traversal into a gfx::Surface,
# and finally presentation of that surface to a real Wayland compositor via wm/wcs.
# Passes iff acl reports the frame was shown (wl_surface.frame callback fired).
#
# SKIPs (exit 77) if Weston can't start in this environment.
#
# Usage: run_view_present_test.py <acl_binary> <view.apk> <framework_dir>
import os
import shutil
import subprocess
import sys
import time

SKIP = 77


def find_weston_libdir():
    for base in ("/usr/local/lib64", "/usr/local/lib", "/usr/lib64", "/usr/lib"):
        if os.path.isdir(base) and any(n.startswith("libweston-") for n in os.listdir(base)):
            mod = os.path.join(base, "weston")
            return ":".join([base] + ([mod] if os.path.isdir(mod) else []))
    return ""


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_view_present_test.py <acl> <view.apk> <fw_dir>\n")
        return 2
    acl, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    weston = shutil.which("weston")
    if not weston:
        sys.stderr.write("SKIP: weston not found\n")
        return SKIP

    runtime_dir = "/tmp/acl-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "acl-wl"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir
    libdir = find_weston_libdir()
    if libdir:
        env["LD_LIBRARY_PATH"] = libdir + ":" + env.get("LD_LIBRARY_PATH", "")

    weston_log = os.path.join(runtime_dir, "weston.log")
    wproc = None
    try:
        with open(weston_log, "wb") as logf:
            wproc = subprocess.Popen(
                [weston, "--backend=headless-backend.so", "--renderer=pixman",
                 "--socket=" + socket, "--width=1920", "--height=1080", "--idle-time=0"],
                env=env, stdout=logf, stderr=subprocess.STDOUT)
        sock_path = os.path.join(runtime_dir, socket)
        for _ in range(50):
            if os.path.exists(sock_path) and wproc.poll() is None:
                break
            time.sleep(0.1)
        if wproc.poll() is not None or not os.path.exists(sock_path):
            sys.stderr.write("SKIP: headless Weston failed to start in this environment\n")
            return SKIP

        penv = dict(env)
        penv["WAYLAND_DISPLAY"] = socket
        p = subprocess.run([acl, "run", apk, "--vm=jvm", "--framework=" + fw, "--present-frames=1"],
                           env=penv, capture_output=True, text=True, timeout=60)
        out = p.stdout + p.stderr
        # The app renders the tree AND presents it; require both the traversal and
        # the compositor-confirmed frame.
        need = ["ViewRoot: performTraversals measured=1920x1080",
                "present loop stopped: 1 frame(s) committed, 1 shown"]
        missing = [s for s in need if s not in out]
        if p.returncode != 0 or missing:
            sys.stderr.write("FAIL: missing %r\n--- output ---\n%s\n" % (missing, out))
            return 1
        sys.stdout.write("OK: view.apk rendered AND presented to headless Weston via wcs\n")
        return 0
    finally:
        if wproc is not None and wproc.poll() is None:
            wproc.terminate()
            try:
                wproc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                wproc.kill()
        shutil.rmtree(runtime_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

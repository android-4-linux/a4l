# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C compositor matrix via a
# headless Weston), Addendum A02 (wcs windowing). No GPL-3.0 sources.
#
# Tier-C wcs presentation test: launch a headless Weston (software/pixman renderer),
# then run the wm_present_probe client, which rasters a gfx::Surface and presents it
# through wm/present (the wcs shm + xdg-shell path). Pass iff the probe reports the
# frame was shown (wl_surface.frame callback fired against a real compositor).
#
# Weston needs a short XDG_RUNTIME_DIR (the wl socket path must fit sun_path's 108
# bytes) and its install lib dir on LD_LIBRARY_PATH. If Weston can't start (missing
# libs/backends in this environment), the test SKIPs (exit 77) rather than fails.
#
# With an optional <frames> argument it instead exercises the CONTINUOUS present
# loop (Choreographer-style on wp_presentation): the probe animates <frames> frames,
# and the test asserts all were committed AND shown AND that wp_presentation feedback
# actually fired (proving the loop is vsync/timing-driven, not a static re-present).
#
# Usage: run_present_test.py <wm_present_probe_binary> [<frames>]
import os
import shutil
import subprocess
import sys
import time

SKIP = 77


def find_weston_libdir():
    # weston is often installed under /usr/local; its libweston-*.so + module dir
    # may not be on the default loader path.
    for base in ("/usr/local/lib64", "/usr/local/lib", "/usr/lib64", "/usr/lib"):
        if os.path.isdir(base) and any(n.startswith("libweston-") for n in os.listdir(base)):
            weston_mod = os.path.join(base, "weston")
            paths = [base]
            if os.path.isdir(weston_mod):
                paths.append(weston_mod)
            return ":".join(paths)
    return ""


def main() -> int:
    if len(sys.argv) not in (2, 3):
        sys.stderr.write("usage: run_present_test.py <probe_binary> [<frames>]\n")
        return 2
    probe = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) == 3 else 0
    weston = shutil.which("weston")
    if not weston:
        sys.stderr.write("SKIP: weston not found\n")
        return SKIP

    runtime_dir = "/tmp/aclcxx-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "aclcxx-wl"
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
                 "--socket=" + socket, "--width=640", "--height=480", "--idle-time=0"],
                env=env, stdout=logf, stderr=subprocess.STDOUT)
        # Wait for the socket to appear (compositor ready), up to ~5 s.
        sock_path = os.path.join(runtime_dir, socket)
        for _ in range(50):
            if os.path.exists(sock_path) and wproc.poll() is None:
                break
            time.sleep(0.1)
        if wproc.poll() is not None or not os.path.exists(sock_path):
            sys.stderr.write("SKIP: headless Weston failed to start in this environment\n")
            try:
                sys.stderr.write(open(weston_log).read()[-800:] + "\n")
            except OSError:
                pass
            return SKIP

        penv = dict(env)
        penv["WAYLAND_DISPLAY"] = socket
        cmd = [probe] + (["--frames", str(frames)] if frames else [])
        p = subprocess.run(cmd, env=penv, capture_output=True, text=True, timeout=45)
        out = p.stdout + p.stderr
        if frames:
            committed = "present loop stopped: %d frame(s) committed, %d shown" % (frames, frames)
            fed_ok = ("presentation-feedback" in out and
                      "0 presentation-feedback" not in out)
            if p.returncode != 0 or "LOOP-PRESENTED-OK" not in out or committed not in out or not fed_ok:
                sys.stderr.write("FAIL: continuous present loop\n--- probe output ---\n" + out + "\n")
                return 1
            sys.stdout.write("OK: %d frames continuously presented to headless Weston "
                             "(wp_presentation feedback fired)\n" % frames)
            return 0
        if p.returncode != 0 or "PRESENTED-OK" not in out:
            sys.stderr.write("FAIL: probe did not present\n--- probe output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: gfx::Surface presented to headless Weston via wcs (frame callback)\n")
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

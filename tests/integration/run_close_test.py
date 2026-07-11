# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), Addendum A02. No GPL-3.0 sources.
#
# Tier-C close-path test: launch the minimal test compositor (close_compositor),
# which drives the presenter through the xdg-shell handshake and then sends
# xdg_toplevel.close once the window maps. Run wm_present_probe UNBOUNDED
# (--frames 0, an animation that never self-stops), so the ONLY thing that can end
# it is the compositor's close. The presenter must react to close (OnToplevelClose)
# and exit cleanly and promptly. If close handling were broken the probe would run
# forever and the harness would time out (FAIL).
#
# Usage: run_close_test.py <wm_present_probe> <close_compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_close_test.py <probe> <compositor>\n")
        return 2
    probe, compositor = sys.argv[1], sys.argv[2]

    runtime_dir = "/tmp/acl-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "acl-close"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir

    cproc = None
    try:
        cproc = subprocess.Popen([compositor, socket], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        # Wait for the compositor socket to appear.
        sock_path = os.path.join(runtime_dir, socket)
        for _ in range(50):
            if os.path.exists(sock_path) and cproc.poll() is None:
                break
            time.sleep(0.1)
        if cproc.poll() is not None or not os.path.exists(sock_path):
            sys.stderr.write("FAIL: test compositor did not start\n")
            return 1

        penv = dict(env)
        penv["WAYLAND_DISPLAY"] = socket
        # --frames 0 => unbounded animation; only a compositor close can stop it.
        # The 30 s timeout is the safety net: if close handling hangs, we fail here.
        p = subprocess.run([probe, "--frames", "0"], env=penv,
                           capture_output=True, text=True, timeout=30)
        out = p.stdout + p.stderr
        if p.returncode != 0 or "toplevel closed" not in out or "LOOP-PRESENTED-OK" not in out:
            sys.stderr.write("FAIL: presenter did not exit cleanly on close\n"
                             "--- probe output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: presenter reacted to xdg_toplevel.close and exited cleanly\n")
        return 0
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: presenter hung — did not react to window close\n")
        return 1
    finally:
        if cproc is not None and cproc.poll() is None:
            cproc.terminate()
            try:
                cproc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cproc.kill()
        shutil.rmtree(runtime_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

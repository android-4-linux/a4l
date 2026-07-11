# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), ADR-6 (windowing/scaling),
# Addendum A02. No GPL-3.0 sources.
#
# Tier-C dynamic-scale test: launch the wm/ test compositor in "dynamic" mode — it
# maps the window at a 1.5x fractional scale (preferred_scale 180 -> 384x384 buffer),
# then, mid-run, sends a new preferred_scale 240 (2x). The presenter must notice the
# runtime change, re-create its shm buffers at the new physical size (512x512) and
# keep the viewport at the logical size — all without tearing the surface down. Then
# the compositor closes. Run wm_present_probe (256x256) and assert both scale steps.
#
# Usage: run_dynamic_test.py <wm_present_probe> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_dynamic_test.py <probe> <compositor>\n")
        return 2
    probe, compositor = sys.argv[1], sys.argv[2]

    runtime_dir = "/tmp/aclcxx-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "aclcxx-dynamic"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir

    cproc = None
    try:
        cproc = subprocess.Popen([compositor, socket, "dynamic"], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
        p = subprocess.run([probe, "--frames", "0"], env=penv,
                           capture_output=True, text=True, timeout=30)
        out = p.stdout + p.stderr
        if (p.returncode != 0
                or "fractional scale 180/120: buffer 384x384 -> viewport 256x256" not in out
                or "fractional scale 240/120: buffer 512x512 -> viewport 256x256" not in out
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: runtime scale change not applied\n--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: runtime scale 1.5x -> 2x re-created buffers, clean close\n")
        return 0
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: run hung\n")
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

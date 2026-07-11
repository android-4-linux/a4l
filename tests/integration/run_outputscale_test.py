# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), ADR-6 (windowing/scaling),
# Addendum A02. No GPL-3.0 sources.
#
# Tier-C integer-HiDPI test: launch the wm/ test compositor in "outputscale" mode —
# it advertises no fractional scale (unity 120) but a wl_output whose integer scale
# is 2. With no fractional scale to honor, the presenter falls back to the output
# scale: it sizes its shm buffer at 2x the logical resolution (256 -> 512) and calls
# wl_surface.set_buffer_scale(2) so the compositor maps it back to the logical size.
# Run wm_present_probe (256x256) and assert the integer-scale path, then a clean close.
#
# Usage: run_outputscale_test.py <wm_present_probe> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_outputscale_test.py <probe> <compositor>\n")
        return 2
    probe, compositor = sys.argv[1], sys.argv[2]

    runtime_dir = "/tmp/acl-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "acl-outputscale"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir

    cproc = None
    try:
        cproc = subprocess.Popen([compositor, socket, "outputscale"], env=env,
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
                or "output scale 2: buffer 512x512 (set_buffer_scale)" not in out
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: integer output scale not applied\n--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: wl_output.scale 2 -> 2x physical buffer + set_buffer_scale, clean close\n")
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

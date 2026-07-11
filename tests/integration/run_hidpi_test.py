# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), ADR-6 (windowing/scaling),
# Addendum A02. No GPL-3.0 sources.
#
# Tier-C HiDPI test: launch the wm/ test compositor in "hidpi" mode — it advertises
# wp_fractional_scale_manager_v1 + wp_viewporter and sends preferred_scale 180
# (= 1.5x, in 1/120 units). The presenter must size its shm buffer at the physical
# resolution (logical 256 -> 384 = ScalePolicy round-half-up of 256*180/120) and set
# the wp_viewport destination back to the logical size. Run wm_present_probe (256x256)
# and assert the buffer/viewport mapping, then a clean close.
#
# Usage: run_hidpi_test.py <wm_present_probe> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_hidpi_test.py <probe> <compositor>\n")
        return 2
    probe, compositor = sys.argv[1], sys.argv[2]

    runtime_dir = "/tmp/aclcxx-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "aclcxx-hidpi"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir

    cproc = None
    try:
        cproc = subprocess.Popen([compositor, socket, "hidpi"], env=env,
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
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: fractional scale not applied\n--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: fractional scale 1.5x -> physical buffer + viewport, clean close\n")
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

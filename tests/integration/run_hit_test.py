# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), Addendum A02. No GPL-3.0 sources.
#
# Tier-C hit-testing test: run `aclcxx run hit.apk --present-frames=0` against the
# wm/ test compositor, which taps at (100,50). hit.apk's content view is a Panel
# (ViewGroup) with two Tiles stacked vertically (top: y 0..540, bottom: 540..1080).
# ViewGroup.dispatchTouchEvent must route the tap to the TOP tile (in its local
# coordinates), so it logs "top" — and the bottom tile is NOT touched. Proves the
# hit-test + coordinate offset in the View tree.
#
# Usage: run_hit_test.py <aclcxx_binary> <hit.apk> <framework_dir> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write("usage: run_hit_test.py <aclcxx> <hit.apk> <fw_dir> <compositor>\n")
        return 2
    aclcxx, apk, fw, compositor = sys.argv[1:5]

    runtime_dir = "/tmp/aclcxx-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "aclcxx-hit"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir

    cproc = None
    try:
        cproc = subprocess.Popen([compositor, socket], env=env,
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
        p = subprocess.run(
            [aclcxx, "run", apk, "--vm=jvm", "--framework=" + fw, "--present-frames=0"],
            env=penv, capture_output=True, text=True, timeout=60)
        out = p.stdout + p.stderr
        # The tap at (100,50) must reach the top tile only.
        if (p.returncode != 0
                or "aclcxx.hit: top" not in out
                or "aclcxx.hit: bottom" in out
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: ViewGroup did not route the tap to the correct child\n"
                             "--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: ViewGroup.dispatchTouchEvent hit-tested the tap to the top tile\n")
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

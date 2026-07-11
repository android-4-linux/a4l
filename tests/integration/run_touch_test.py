# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), Addendum A02. No GPL-3.0 sources.
#
# Tier-C input test: run `aclcxx run touch.apk --present-frames=0` against the wm/
# test compositor, which (once the window maps) injects a pointer tap and then
# closes. The tap must flow wl_pointer -> presenter -> MotionEvent ->
# View.dispatchTouchEvent -> TouchView.onTouchEvent, which logs "onTouchEvent" via
# android.util.Log. The test asserts that line appears (input reached the app) and
# that aclcxx then exits cleanly on the close.
#
# Usage: run_touch_test.py <aclcxx_binary> <touch.apk> <framework_dir> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write("usage: run_touch_test.py <aclcxx> <touch.apk> <fw_dir> <compositor>\n")
        return 2
    aclcxx, apk, fw, compositor = sys.argv[1:5]

    runtime_dir = "/tmp/aclcxx-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "aclcxx-touch"
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
        # The app logs from onTouchEvent (proof the tap reached the View), and the
        # run exits cleanly when the compositor then closes the window.
        if (p.returncode != 0
                or "launching Activity com.aclcxx.hello.MainActivity" not in out
                or "aclcxx.touch: onTouchEvent" not in out
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: touch input did not reach the app\n--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: pointer tap -> MotionEvent -> TouchView.onTouchEvent, then clean close\n")
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

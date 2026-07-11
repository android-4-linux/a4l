# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), ADR-6 (windowing), §2
# (svc WindowManager/Display). No GPL-3.0 sources.
#
# Tier-C display-metrics test: launch the wm/ test compositor in "display" mode — it
# advertises a wl_output whose active mode is 1280x720. acl must read that mode
# (wm::query_display) before onCreate and seed svc::WindowService, so the app's
# getWindowManager()/getDefaultDisplay() reports 1280x720 instead of the built-in
# 1080p default. Runs any Activity (touch.apk) --present and asserts the display line.
#
# Usage: run_display_test.py <acl> <apk> <fw_dir> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write("usage: run_display_test.py <acl> <apk> <fw_dir> <compositor>\n")
        return 2
    acl, apk, fw, compositor = sys.argv[1:5]

    runtime_dir = "/tmp/acl-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "acl-display"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir

    cproc = None
    try:
        cproc = subprocess.Popen([compositor, socket, "display"], env=env,
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
            [acl, "run", apk, "--vm=jvm", "--framework=" + fw, "--present-frames=0"],
            env=penv, capture_output=True, text=True, timeout=60)
        out = p.stdout + p.stderr
        # The wl_output mode drives both the "display from wl_output" seed log and the
        # app-visible Display metrics reported by probe_managers.
        if (p.returncode != 0
                or "display from wl_output: 1280x720" not in out
                or "WindowManager: display=1280x720" not in out
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: display metrics not sourced from wl_output\n--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: wl_output mode 1280x720 -> android.view.Display, clean close\n")
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

# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), Addendum A02. No GPL-3.0 sources.
#
# Tier-C key-repeat test: run `acl run key.apk --present-frames=0` against the wm/
# test compositor in "repeat" mode — it advertises wl_keyboard.repeat_info and holds
# KEY_A (no release), then closes after ~500 ms. The presenter's loop must poll the
# wcs KeyboardHandler repeat timerfd and DispatchRepeat, so the held key fires OnKey
# repeatedly -> KeyView.onKeyDown runs multiple times. The test asserts >= 2
# "onKeyDown" log lines (initial press + at least one timerfd repeat), then a clean
# close.
#
# Usage: run_key_repeat_test.py <acl_binary> <key.apk> <framework_dir> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write("usage: run_key_repeat_test.py <acl> <key.apk> <fw_dir> <compositor>\n")
        return 2
    acl, apk, fw, compositor = sys.argv[1:5]

    runtime_dir = "/tmp/acl-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "acl-keyrep"
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = runtime_dir

    cproc = None
    try:
        cproc = subprocess.Popen([compositor, socket, "repeat"], env=env,
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
        downs = out.count("acl.key: onKeyDown")
        if p.returncode != 0 or downs < 2 or "toplevel closed" not in out:
            sys.stderr.write("FAIL: key repeat did not reach the app (onKeyDown x%d)\n"
                             "--- output ---\n%s\n" % (downs, out))
            return 1
        sys.stdout.write("OK: held key repeated via the timerfd -> onKeyDown x%d, then clean close\n"
                         % downs)
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

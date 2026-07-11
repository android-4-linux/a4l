# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), Addendum A02. No GPL-3.0 sources.
#
# Tier-C keyboard test: run `acl run key.apk --present-frames=0` against the wm/
# test compositor, which (once the window maps) advertises a keyboard and injects a
# key press (KEY_A), then closes. The key must flow wl_keyboard -> presenter (wcs
# KeyboardHandler) -> KeyEvent -> View.dispatchKeyEvent -> KeyView.onKeyDown, which
# logs "onKeyDown" via android.util.Log. The test asserts that line appears (the key
# reached the app) and that acl then exits cleanly on the close.
#
# Usage: run_key_test.py <acl_binary> <key.apk> <framework_dir> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write("usage: run_key_test.py <acl> <key.apk> <fw_dir> <compositor>\n")
        return 2
    acl, apk, fw, compositor = sys.argv[1:5]

    runtime_dir = "/tmp/acl-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "acl-key"
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
            [acl, "run", apk, "--vm=jvm", "--framework=" + fw, "--present-frames=0"],
            env=penv, capture_output=True, text=True, timeout=60)
        out = p.stdout + p.stderr
        if (p.returncode != 0
                or "launching Activity com.acl.hello.MainActivity" not in out
                or "acl.key: onKeyDown" not in out
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: key input did not reach the app\n--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: key press -> KeyEvent -> KeyView.onKeyDown, then clean close\n")
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

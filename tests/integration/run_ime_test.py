# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier C), Addendum A02. No GPL-3.0 sources.
#
# Tier-C IME test: run `aclcxx run ime.apk --present-frames=0` against the wm/ test
# compositor, which acts as an IME (zwp_text_input_v3). ime.apk's ImeView returns an
# InputConnection, so the presenter enables Wayland text input; once the window maps
# the compositor sends commit_string "hello". That must flow zwp_text_input_v3 ->
# wcs TextInputV3 facade -> presenter -> InputConnection.commitText -> ImeConn logs
# "commitText". Asserts the native commit line AND the app-side dispatch, then a
# clean close.
#
# Usage: run_ime_test.py <aclcxx_binary> <ime.apk> <framework_dir> <compositor>
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write("usage: run_ime_test.py <aclcxx> <ime.apk> <fw_dir> <compositor>\n")
        return 2
    aclcxx, apk, fw, compositor = sys.argv[1:5]

    runtime_dir = "/tmp/aclcxx-wl-%d" % os.getpid()
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    socket = "aclcxx-ime"
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
        if (p.returncode != 0
                or "launching Activity com.aclcxx.hello.MainActivity" not in out
                or "aclcxx.ime: deleteSurroundingText" not in out  # delete_surrounding_text
                or 'IME preedit: "hel"' not in out           # presenter received preedit_string
                or 'IME committed: "hello"' not in out       # ...and commit_string
                or "aclcxx.ime: setComposingText" not in out # dispatched to setComposingText
                or "aclcxx.ime: commitText" not in out       # ...and commitText
                or "toplevel closed" not in out):
            sys.stderr.write("FAIL: IME text did not reach the app\n--- output ---\n" + out + "\n")
            return 1
        sys.stdout.write("OK: IME delete/preedit/commit -> InputConnection, clean close\n")
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

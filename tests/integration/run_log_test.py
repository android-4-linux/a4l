# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/framework-bootstrap.md
# Gate G2.b. No GPL-3.0 sources.
#
# Prove the JNI native underside: an app calls android.util.Log.i(tag, msg); the
# framework Log class (on the boot classpath) funnels to a native write0() that
# aclcxx implements via RegisterNatives, forwarding to aclcxx::log. So the app's
# log line must appear in aclcxx::log's own "I/<tag>: <msg>" format — which only
# happens if the whole android.* -> JNI -> aclcxx native core seam fired.
#
# Usage: run_log_test.py <aclcxx_binary> <log.apk> <framework_dir>
import subprocess
import sys

BOUND = "framework native bound: android.util.Log"   # RegisterNatives succeeded
LINE = "I/aclcxx.app: hello via android.util.Log"     # aclcxx::log emitted it


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_log_test.py <aclcxx> <log.apk> <fw_dir>\n")
        return 2
    aclcxx, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    p = subprocess.run([aclcxx, "run", apk, "--vm=jvm", "--framework=" + fw],
                       capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = True
    if p.returncode != 0:
        sys.stderr.write(f"FAIL: exit code {p.returncode}\n"); ok = False
    if BOUND not in out:
        sys.stderr.write("FAIL: android.util.Log native was not bound\n"); ok = False
    if LINE not in out:
        sys.stderr.write("FAIL: the app's Log.i() did not reach aclcxx::log\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: app -> android.util.Log -> JNI native -> aclcxx::log\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

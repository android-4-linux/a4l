# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/framework-bootstrap.md
# Gate G2.a. No GPL-3.0 sources.
#
# Prove the framework boot-classpath seam: an APK whose main() calls
# com.aclcxx.fw.Probe.tag() — a class NOT bundled in the APK — runs only when Probe
# is supplied via --framework. With the framework it prints the probe tag; without
# it, class resolution must fail. This is the mechanism the vendored android.*
# framework will use.
#
# Usage: run_framework_test.py <aclcxx_binary> <probe.apk> <framework_dir>
import subprocess
import sys

TAG = "aclcxx: framework probe on boot classpath"


def run(argv):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout, p.stderr


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_framework_test.py <aclcxx> <probe.apk> <fw_dir>\n")
        return 2
    aclcxx, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    ok = True

    # (1) With the framework on the boot classpath: resolves Probe, prints its tag.
    rc, out, err = run([aclcxx, "run", apk, "--vm=jvm", "--framework=" + fw])
    if rc != 0 or TAG not in out:
        sys.stderr.write("FAIL: with --framework, expected probe tag and exit 0\n")
        sys.stderr.write("--- stdout ---\n" + out + "\n--- stderr ---\n" + err + "\n")
        ok = False

    # (2) Without it: Probe is nowhere on the classpath, so the app must fail (the
    # tag must NOT print). This confirms Probe genuinely came from --framework.
    rc2, out2, err2 = run([aclcxx, "run", apk, "--vm=jvm"])
    if TAG in out2 or rc2 == 0:
        sys.stderr.write("FAIL: without --framework, the app should NOT resolve Probe\n")
        sys.stderr.write("--- stdout ---\n" + out2 + "\n--- stderr ---\n" + err2 + "\n")
        ok = False

    if not ok:
        return 1
    sys.stdout.write("OK: app resolved a boot-classpath-only class via --framework\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

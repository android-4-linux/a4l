# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B integration). No GPL-3.0 sources.
#
# End-to-end run-path test: exec the real `acl` binary on a hello-world APK and
# assert it (1) resolves the launcher activity from the binary manifest, (2)
# converts the DEX to JVM classes, and (3) actually executes the translated
# main(), printing the expected line. This exercises the whole vertical
# CLI -> pkg -> manifest -> dex -> DEX/JVM translate -> JVM -> app output.
#
# Usage: run_apk_test.py <acl_binary> <hello.apk>
import subprocess
import sys

EXPECTED = "acl: hello from a real APK"
LAUNCH = "com.acl.hello.Main"


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_apk_test.py <acl_binary> <hello.apk>\n")
        return 2
    acl, apk = sys.argv[1], sys.argv[2]

    # No --main and no --convert-dex: the launcher must come from the manifest and
    # DEX conversion must be implied by --vm=jvm.
    proc = subprocess.run(
        [acl, "run", apk, "--vm=jvm"],
        capture_output=True, text=True, timeout=120)
    out = proc.stdout + proc.stderr

    ok = True
    if proc.returncode != 0:
        sys.stderr.write(f"FAIL: exit code {proc.returncode}\n")
        ok = False
    if LAUNCH not in out:
        sys.stderr.write(f"FAIL: launcher '{LAUNCH}' not resolved from manifest\n")
        ok = False
    if EXPECTED not in proc.stdout:
        sys.stderr.write(f"FAIL: expected app output '{EXPECTED}' not printed\n")
        ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write(f"OK: launcher resolved, DEX converted, app printed '{EXPECTED}'\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

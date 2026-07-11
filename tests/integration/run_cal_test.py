# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §A01.3 (automotive CAL path, G1.5b),
# ADR-7 / A02.3 (native CAL host). No GPL-3.0 sources.
#
# Prove the automotive Car App Library entry point: cal.apk's CarApp extends
# androidx.car.app.CarAppService (shims on the boot classpath). Hosted via
# `acl run --cal=...`, acl must instantiate it, onCreateSession() -> onCreateScreen()
# -> onGetTemplate(), then feed the returned ListTemplate to the native CAL host,
# which renders it (header + rows) and enforces the content limit — in-process, no
# Views, no Binder.
#
# Usage: run_cal_test.py <acl_binary> <cal.apk> <framework_dir>
import subprocess
import sys

CHECKS = [
    "hosting CarAppService com.acl.hello.CarApp",
    # The native CAL host rendered the app's ListTemplate (2 rows, under the limit).
    "CalHost: rendered [List] 'Media' — 2 row(s), limit 6, truncated=false",
    "[List] Media",
    "• Play Music",       # bulleted row 1
    "• Browse Library",   # bulleted row 2
]


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_cal_test.py <acl> <cal.apk> <fw_dir>\n")
        return 2
    acl, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    p = subprocess.run(
        [acl, "run", apk, "--vm=jvm", "--framework=" + fw,
         "--cal=com.acl.hello.CarApp"],
        capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n")
    for c in CHECKS:
        if c not in out:
            sys.stderr.write(f"FAIL: missing '{c}'\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: CarAppService hosted; template walked + rendered by the CAL host\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

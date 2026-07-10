# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/framework-bootstrap.md
# Gate G2.d. No GPL-3.0 sources.
#
# Prove the Android Activity entry point: activity.apk's launcher is
# com.aclcxx.hello.MainActivity extends android.app.Activity (a shim on the boot
# classpath). `aclcxx run` must resolve the launcher from the manifest, recognize
# it as an Activity subclass, instantiate it, and drive onCreate -> onStart ->
# onResume — NOT fall back to a static main() (there is none). onCreate calls
# super.onCreate() and android.util.Log.i(), so its line surfaces via aclcxx::log.
#
# Usage: run_activity_test.py <aclcxx_binary> <activity.apk> <framework_dir>
import subprocess
import sys

LAUNCHING = "launching Activity com.aclcxx.hello.MainActivity"  # lifecycle path taken
CONTEXT = "entry point is a Context: getPackageName()"          # Activity extends Context
ONCREATE = "MainActivity.onCreate ran"                          # onCreate executed
SVC = "set stream 3 volume -> 7"  # getSystemService(AUDIO)->AudioManager->svc hub


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_activity_test.py <aclcxx> <activity.apk> <fw_dir>\n")
        return 2
    aclcxx, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    p = subprocess.run([aclcxx, "run", apk, "--vm=jvm", "--framework=" + fw],
                       capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = True
    if p.returncode != 0:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n"); ok = False
    if LAUNCHING not in out:
        sys.stderr.write("FAIL: driver did not take the Activity-lifecycle path\n"); ok = False
    if CONTEXT not in out:
        sys.stderr.write("FAIL: Activity is not a functioning Context\n"); ok = False
    if ONCREATE not in out:
        sys.stderr.write("FAIL: MainActivity.onCreate did not run\n"); ok = False
    if SVC not in out:
        sys.stderr.write("FAIL: getSystemService(AUDIO) did not reach the svc hub\n"); ok = False
    if "is not an Activity" in out or "invoking static main" in out:
        sys.stderr.write("FAIL: wrongly fell back to static main()\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: launcher Activity instantiated and onCreate driven (no static main)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

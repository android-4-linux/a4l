# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; ADR-0002 (android.jar stub boot layer);
# docs/framework-bootstrap.md §B. No GPL-3.0 sources.
#
# ADR-0002 android.jar layer: an app references android.util.SparseArray — a class the
# acl shims do NOT provide — via const-class + Class.getName(). With NO --framework it
# must resolve from the staged android.jar stub layer (baked into the default boot
# classpath beneath the shims) and print the class name. This proves the whole android.*
# public surface is now on the boot classpath, not just the hand-written shims.
#
# Usage: run_android_jar_test.py <acl_binary> <stub.apk>
import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_android_jar_test.py <acl> <stub.apk>\n")
        return 2
    acl, apk = sys.argv[1], sys.argv[2]

    env = dict(os.environ)
    env.pop("ACL_FRAMEWORK", None)  # force the baked-in default (shims + android.jar)
    p = subprocess.run([acl, "run", apk, "--vm=jvm"],  # no --framework
                       env=env, capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n")
    if "android.util.SparseArray" not in out:
        # A non-shimmed android.* class did not resolve -> the android.jar layer is
        # missing from the default boot classpath.
        sys.stderr.write("FAIL: android.util.SparseArray did not resolve from android.jar\n")
        ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: a non-shimmed android.* class resolved from the android.jar stub layer\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

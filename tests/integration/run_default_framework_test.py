# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; ADR-0002 (default boot classpath);
# docs/framework-bootstrap.md §A1; plan.md §4. No GPL-3.0 sources.
#
# ADR-0002 default boot classpath: `acl run <apk>` with NO --framework and no
# $ACL_FRAMEWORK must still resolve android.* — the shell falls back to the staged
# framework baked in at build time (ACL_DEFAULT_FRAMEWORK -> build/framework/classes).
# Runs activity.apk (MainActivity extends android.app.Activity, so it needs the
# framework to even load) and asserts the lifecycle ran off the default classpath.
#
# Usage: run_default_framework_test.py <acl_binary> <activity.apk>
import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_default_framework_test.py <acl> <activity.apk>\n")
        return 2
    acl, apk = sys.argv[1], sys.argv[2]

    env = dict(os.environ)
    env.pop("ACL_FRAMEWORK", None)  # force the baked-in default, not an env override
    p = subprocess.run([acl, "run", apk, "--vm=jvm"],  # note: no --framework
                       env=env, capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n")
    checks = [
        "framework boot classpath:",                     # a default was picked up
        "build/framework/classes",                       # ...including the staged shim dir
        "launching Activity com.acl.hello.MainActivity",  # loaded (needs android.*)
        "MainActivity.onCreate ran",                     # lifecycle drove onCreate
    ]
    for c in checks:
        if c not in out:
            sys.stderr.write(f"FAIL: missing '{c}'\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: `acl run` resolved android.* from the default framework (no --framework)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

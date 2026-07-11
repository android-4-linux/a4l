# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; ADR-0002 (framework vendoring); plan.md §4
# (subsetting tooling), §3 R-2. No GPL-3.0 sources.
#
# Exercise tools/vendor_framework.py against the synthetic AOSP-shaped fixture (same
# tree as fw_extract_test), so the size/vendor/manifest logic is verified without a
# real AOSP checkout. Fixture closure of android.util = 4 classes with one denylist
# edge (android.text.Span -> com.android.server.TextService).
#
# Usage: vendor_framework_test.py <vendor_framework.py> <fwsrc_dir> <tmp_dest>
import os
import shutil
import subprocess
import sys


def run(argv):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: vendor_framework_test.py <tool> <fwsrc> <tmp_dest>\n")
        return 2
    tool, src, dest = sys.argv[1], sys.argv[2], sys.argv[3]
    base = [sys.executable, tool, "--aosp-root", src, "--src-rel", ".", "--seed", "android.util"]
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            sys.stderr.write("FAIL: " + msg + "\n")
            ok = False

    # (1) size: 4-class closure + the SystemServer denylist edge (the shim boundary).
    rc, out = run(base + ["size"])
    check(rc == 0, f"size exit {rc}: {out}")
    check("closure=4 class(es)" in out, "size should report a 4-class closure")
    check("android.text.Span  ->  com.android.server.TextService" in out,
          "size should flag the denylist edge")

    # (2) budget under the closure size must refuse to vendor.
    rc, out = run(base + ["vendor", "--dest", dest, "--budget", "2"])
    check(rc == 1 and "exceeds budget" in out, "vendor should refuse over-budget")

    # (3) vendor for real: copies the 4 files (package paths) + writes MANIFEST rows.
    shutil.rmtree(dest, ignore_errors=True)
    rc, out = run(base + ["vendor", "--dest", dest])
    check(rc == 0, f"vendor exit {rc}: {out}")
    check("vendored 4 class(es)" in out, "vendor should copy 4 classes")
    for rel in ("android/util/Helper.java", "android/util/Widget.java",
                "android/text/Span.java", "android/text/Style.java"):
        check(os.path.isfile(os.path.join(dest, rel)), f"missing vendored file {rel}")
    manifest = os.path.join(dest, "MANIFEST.md")
    check(os.path.isfile(manifest), "MANIFEST.md not written")
    if os.path.isfile(manifest):
        m = open(manifest, encoding="utf-8").read()
        check("`android.util.Widget`" in m and "android-13.0.0_r84" in m,
              "MANIFEST.md missing the vendored rows / tag")
    shutil.rmtree(dest, ignore_errors=True)

    if not ok:
        return 1
    sys.stdout.write("OK: vendor_framework size/budget/vendor + MANIFEST verified\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

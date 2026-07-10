# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §4 (subsetting tooling), §3 R-2. No GPL-3.0 sources.
#
# Exercise tools/fw_extract.py against a synthetic AOSP-shaped source tree so the
# closure/budget/denylist logic is verified deterministically (no real AOSP needed).
# The fixture: android.util.{Widget,Helper} seed; Widget imports android.text.Span;
# Helper wildcard-imports android.text.* (Span, Style); Span imports the denylisted
# com.android.server.TextService.
#
# Usage: fw_extract_test.py <fw_extract.py> <fwsrc_dir>
import json
import subprocess
import sys


def run(argv):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout, p.stderr


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: fw_extract_test.py <fw_extract.py> <fwsrc_dir>\n")
        return 2
    tool, src = sys.argv[1], sys.argv[2]
    base = [sys.executable, tool, "--src", src]
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            sys.stderr.write("FAIL: " + msg + "\n")
            ok = False

    # (1) Default denylist: closure prunes at the SystemServer boundary and flags it.
    rc, out, err = run(base + ["--seed", "android.util", "--json"])
    check(rc == 2, f"expected exit 2 (denylist reached), got {rc}: {err}")
    try:
        d = json.loads(out)
    except json.JSONDecodeError:
        check(False, "tool did not emit valid JSON"); d = {}
    check(d.get("closure_classes") == 4, f"closure should be 4, got {d.get('closure_classes')}")
    check(d.get("per_package", {}).get("android.util") == 2, "android.util should have 2")
    check(d.get("per_package", {}).get("android.text") == 2, "android.text should have 2")
    edges = {(e["from"], e["reaches"]) for e in d.get("denylist_edges", [])}
    check(("android.text.Span", "com.android.server.TextService") in edges,
          f"missing Span->TextService denylist edge; got {edges}")
    # Boundary pruning: the denylisted class is NOT pulled into the closure.
    check("com.android.server" not in d.get("per_package", {}),
          "denylisted package should not be in the closure")

    # (2) Budget path: with the denylist disabled, TextService IS followed (closure
    # grows to 5) and the total budget of 2 is exceeded -> exit 1, no denylist edge.
    rc2, out2, _ = run(base + ["--seed", "android.util", "--budget", "2",
                               "--denylist", "zzz.never", "--json"])
    check(rc2 == 1, f"expected exit 1 (budget), got {rc2}")
    d2 = json.loads(out2) if out2.strip().startswith("{") else {}
    check(d2.get("over_total_budget") is True, "over_total_budget should be true")
    check(d2.get("closure_classes") == 5, f"closure should be 5 w/o denylist, got {d2.get('closure_classes')}")

    # (3) Clean path: no budget, denylist disabled -> exit 0.
    rc3, _, _ = run(base + ["--seed", "android.text", "--denylist", "zzz.never"])
    check(rc3 == 0, f"expected exit 0 (clean), got {rc3}")

    if not ok:
        return 1
    sys.stdout.write("OK: closure, per-package counts, budget, and denylist boundary all correct\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

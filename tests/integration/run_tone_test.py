# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §A01.2 (audio slice), §5 (Tier B).
# No GPL-3.0 sources.
#
# Smoke-test the `acl tone` manual audio-check command: generate a short sine and
# push it through the audio backend. Run with ACL_AUDIO=null so it is silent and
# deterministic (no device, no daemon) — this exercises tone generation, the write
# loop, drain, and frame accounting, and asserts the command terminates promptly
# (a hang trips the harness timeout). The audible PipeWire path is `acl tone` with a
# daemon present; that is a manual check, not asserted here.
#
# Usage: run_tone_test.py <acl_binary>
import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: run_tone_test.py <acl>\n")
        return 2
    acl = sys.argv[1]

    env = dict(os.environ)
    env["ACL_AUDIO"] = "null"  # silent + deterministic for the automated check
    # 0.1s @ 24000 Hz stereo = 2400 frames.
    try:
        p = subprocess.run(
            [acl, "tone", "--seconds=0.1", "--rate=24000", "--channels=2", "--freq=660"],
            env=env, capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: `acl tone` hung\n")
        return 1
    out = p.stdout + p.stderr

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n")
    for c in ("tone: 660 Hz sine", "tone: done, 2400 frames written"):
        if c not in out:
            sys.stderr.write(f"FAIL: missing '{c}'\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: `acl tone` generated + accounted for 2400 frames (silent null sink)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

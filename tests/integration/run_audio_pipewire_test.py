# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §A01.2 (audio slice, pulled forward for
# the media bridge / G1.5a), §4 Phase 3 (AudioTrack->PipeWire), §5 (Tier B). No GPL-3.0 sources.
#
# Prove the PipeWire audio backend end-to-end: audio.apk writes a 1s tone via
# android.media.AudioTrack; with ACL_AUDIO=pipewire the acl native underside routes
# the PCM to the PipeWire sink, which streams it to the running daemon. The path must
# always terminate quickly and account for all 44100 frames:
#   * when a PipeWire daemon socket is present -> it actually streams (sink=pipewire);
#   * when no daemon is reachable -> open() fails within a bounded budget and falls
#     back to the deterministic null sink (never hangs).
# This exercises the pulled-forward audio slice the media bridge (G1.5a) depends on.
#
# Usage: run_audio_pipewire_test.py <acl_binary> <audio.apk> <framework_dir>
import os
import subprocess
import sys


def daemon_present() -> bool:
    rt = os.environ.get("XDG_RUNTIME_DIR")
    remote = os.environ.get("PIPEWIRE_REMOTE", "pipewire-0")
    return bool(rt) and os.path.exists(os.path.join(rt, remote))


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_audio_pipewire_test.py <acl> <audio.apk> <fw_dir>\n")
        return 2
    acl, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]

    env = dict(os.environ)
    env["ACL_AUDIO"] = "pipewire"
    try:
        p = subprocess.run([acl, "run", apk, "--vm=jvm", "--framework=" + fw],
                           env=env, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: PipeWire audio path hung (no bounded connect/drain)\n")
        return 1
    out = p.stdout + p.stderr

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n")
    # All frames must be accounted for regardless of which sink served them.
    if "44100 frames written" not in out:
        sys.stderr.write("FAIL: audio sink did not receive 44100 frames\n"); ok = False

    if daemon_present():
        # A daemon is reachable: the PipeWire backend must actually stream.
        if "sink=pipewire open" not in out:
            sys.stderr.write("FAIL: PipeWire daemon present but backend did not stream\n")
            ok = False
        msg = "streamed to the PipeWire daemon"
    else:
        # No daemon: the backend must fail-open gracefully to the null sink.
        if "falling back to null sink" not in out and "built without PipeWire" not in out:
            sys.stderr.write("FAIL: no daemon but no graceful fallback\n"); ok = False
        msg = "no daemon; fell back to null sink cleanly"

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write(f"OK: app -> AudioTrack -> PipeWire sink ({msg})\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

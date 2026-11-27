# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; after run_audio_pipewire_test.py. No GPL-3.0 sources.
#
# The ALSA audio backend from an app: audio.apk writes a 1 s tone through
# android.media.AudioTrack, and the samples must reach an ALSA PCM in two cases:
#   * ACL_AUDIO=alsa: ALSA asked for by name;
#   * ACL_AUDIO=pipewire with no PipeWire daemon to be found (PIPEWIRE_REMOTE names a
#     socket that is not there): the fallback a board with no PipeWire relies on.
# ACL_ALSA_DEVICE=null picks alsa-lib's null PCM, so the calls are real and there is no sound
# and no sound card needed. All 44100 frames must be accounted for and the run must end.
#
# Usage: run_audio_alsa_test.py <acl_binary> <audio.apk> <framework_dir>
import os
import subprocess
import sys

CASES = [
    ("ACL_AUDIO=alsa", {"ACL_AUDIO": "alsa"}, []),
    ("ACL_AUDIO=pipewire, no daemon",
     {"ACL_AUDIO": "pipewire", "PIPEWIRE_REMOTE": "a4l-test-no-such-daemon"},
     ["audio goes to ALSA"]),
]


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_audio_alsa_test.py <acl> <audio.apk> <fw_dir>\n")
        return 2
    acl, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    for name, extra, more in CASES:
        env = dict(os.environ, ACL_ALSA_DEVICE="null", **extra)
        try:
            p = subprocess.run([acl, "run", apk, "--vm=jvm", "--framework=" + fw],
                               env=env, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            sys.stderr.write(f"FAIL: {name}: the run did not end\n")
            return 1
        out = p.stdout + p.stderr
        want = ["sink=alsa open: 44100 Hz", "device 'null'",
                "sink=alsa close: 44100 frames written"] + more
        missing = [w for w in want if w not in out]
        if p.returncode != 0 or missing:
            sys.stderr.write(f"FAIL: {name}: exit {p.returncode}, missing {missing}\n"
                             f"--- captured output ---\n{out}\n")
            return 1
    sys.stdout.write("OK: app -> AudioTrack -> ALSA PCM, asked for by name and as the "
                     "fallback with no PipeWire daemon\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

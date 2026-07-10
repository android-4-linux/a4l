# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §A01.2 (audio slice), §5 (Tier B),
# docs/framework-bootstrap.md Gate G2.e. No GPL-3.0 sources.
#
# Prove the audio output path: audio.apk's main() generates a 1s tone into a
# short[] and writes it via android.media.AudioTrack; the aclcxx native underside
# forwards the PCM to an aclcxx::audio::Sink. Runs with the deterministic counting
# sink (no ACLCXX_AUDIO), so it asserts exactly 44100 frames independent of any
# audio device. (The PipeWire backend — ACLCXX_AUDIO=pipewire — actually plays it.)
#
# Usage: run_audio_test.py <aclcxx_binary> <audio.apk> <framework_dir>
import subprocess
import sys

BOUND = "framework native bound: android.media.AudioTrack"
FRAMES = "44100 frames written"


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_audio_test.py <aclcxx> <audio.apk> <fw_dir>\n")
        return 2
    aclcxx, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    # No ACLCXX_AUDIO -> the counting/null sink (deterministic, no device).
    p = subprocess.run([aclcxx, "run", apk, "--vm=jvm", "--framework=" + fw],
                       capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n")
    if BOUND not in out:
        sys.stderr.write("FAIL: android.media.AudioTrack native not bound\n"); ok = False
    if FRAMES not in out:
        sys.stderr.write("FAIL: audio sink did not receive 44100 frames\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: app -> AudioTrack -> JNI native -> aclcxx::audio (44100 frames)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

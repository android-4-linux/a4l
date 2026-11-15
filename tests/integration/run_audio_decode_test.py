# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; FFmpeg CLI to encode the fixture tone; the probe
# src/media/audio_decode_probe_main.cpp. No GPL-3.0 sources.
#
# media::AudioDecoder, the engine under android.media.MediaCodec for audio, on each codec a4l
# names a decoder for. A 1 s 440 Hz stereo tone is encoded with the ffmpeg CLI, cut into access
# units by the probe as an Android player would receive them (with the codec-specific data a
# MediaFormat carries), decoded, and measured: the sample rate and channel count, the length,
# the frequency, the level, and that the stream ended.
#
#   audio/mp4a-latm  AAC in ADTS        csd-0 = AudioSpecificConfig
#   audio/mpeg       MP3
#   audio/opus       Opus in Ogg        csd-0 = OpusHead
#   audio/vorbis     Vorbis in Ogg      csd-0 = identification, csd-1 = setup
#   audio/flac       FLAC in Ogg        csd-0 = "fLaC" + STREAMINFO
#
# A codec whose encoder the ffmpeg CLI lacks, or whose decoder the FFmpeg libraries lack, is
# skipped and named. The test SKIPs (exit 77) only if none could be run.
import os
import re
import shutil
import subprocess
import sys
import tempfile

SKIP = 77
HZ = 440

# mime, file, encoder arguments, sample rate, lossless
CODECS = [
    ("audio/mp4a-latm", "tone.aac", ["-c:a", "aac", "-b:a", "128k", "-f", "adts"], 48000, False),
    ("audio/mpeg", "tone.mp3", ["-c:a", "libmp3lame", "-b:a", "128k", "-f", "mp3"], 44100, False),
    ("audio/opus", "tone.opus", ["-c:a", "libopus", "-b:a", "96k", "-f", "ogg"], 48000, False),
    ("audio/vorbis", "tone.ogg", ["-c:a", "libvorbis", "-q:a", "4", "-f", "ogg"], 48000, False),
    ("audio/flac", "tone.oga", ["-c:a", "flac", "-f", "ogg"], 48000, True),
]


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: run_audio_decode_test.py <acl-audio-decode-probe>\n")
        return 2
    probe = sys.argv[1]
    if not os.access(probe, os.X_OK):
        sys.stdout.write("SKIP: probe not built\n")
        return SKIP
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        sys.stdout.write("SKIP: ffmpeg CLI not found (encodes the fixture)\n")
        return SKIP

    work = tempfile.mkdtemp(prefix="a4l-adec-")
    ran, skipped, failures = [], [], []
    try:
        for mime, name, enc, rate, lossless in CODECS:
            path = os.path.join(work, name)
            g = subprocess.run(
                [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                 "-i", f"sine=frequency={HZ}:sample_rate={rate}:duration=1", "-ac", "2"]
                + enc + [path], capture_output=True, text=True)
            if g.returncode != 0 or not os.path.getsize(path):
                skipped.append(f"{mime} (no encoder in the ffmpeg CLI)")
                continue
            p = subprocess.run([probe, mime, path], capture_output=True, text=True, timeout=60)
            out = p.stdout + p.stderr
            if p.returncode == SKIP:
                skipped.append(f"{mime} (no decoder in the FFmpeg libraries)")
                continue
            m = re.search(r"units=(\d+) csd=(\d+) frames=(\d+) rate=(\d+) channels=(\d+) "
                          r"format_changes=(\d+) seconds=([\d.]+) hz=([\d.]+) peak=(\d+) eos=(\d)",
                          out)
            if p.returncode != 0 or not m:
                failures.append(f"{mime}: the probe failed (exit {p.returncode})\n{out[-1500:]}")
                continue
            units, csd, frames, got_rate, channels, changes = (int(m.group(i)) for i in range(1, 7))
            seconds, hz, peak, eos = float(m.group(7)), float(m.group(8)), int(m.group(9)), m.group(10)
            problems = []
            if got_rate != rate:
                problems.append(f"rate {got_rate}, expected {rate}")
            if channels != 2:
                problems.append(f"{channels} channel(s), expected 2")
            if changes != 1:
                problems.append(f"the format changed {changes} times")
            # An encoder pads the start and end by up to a few frames; a lossless one does not.
            if lossless and frames != rate:
                problems.append(f"{frames} frames, expected exactly {rate}")
            if not 0.98 <= seconds <= 1.15:
                problems.append(f"{seconds} s, expected about 1")
            if abs(hz - HZ) > 3:
                problems.append(f"{hz} Hz, expected {HZ}")
            # The CLI's tone is an eighth of full scale, less 3 dB for the stereo upmix.
            if not 2300 <= peak <= 3500:
                problems.append(f"peak {peak}, expected about 2900")
            if eos != "1":
                problems.append("the stream did not end")
            if problems:
                failures.append(f"{mime}: " + "; ".join(problems) + f"\n{out[-800:]}")
            else:
                ran.append(f"{mime} {units} units -> {frames} frames at {got_rate} Hz, "
                           f"{hz:.1f} Hz")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        sys.stderr.write("FAIL:\n" + "\n".join(failures) + "\n")
        return 1
    if not ran:
        sys.stdout.write("SKIP: " + "; ".join(skipped) + "\n")
        return SKIP
    sys.stdout.write("OK: " + "; ".join(ran)
                     + ("\nnot run: " + "; ".join(skipped) if skipped else "") + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

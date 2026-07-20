# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; FFmpeg CLI (lavfi color source) for the fixture clip; the
# zero-copy video-decode probe (src/media/video_decode_probe_main.cpp). No GPL-3.0 sources.
#
# The zero-copy hardware-decode path end-to-end ([[flutter-texture-bridge]] M5): generate a
# solid-color H.264 clip, then acl-video-decode-probe VAAPI-decodes it, maps each frame to a
# DRM_PRIME dma-buf, ahb_imports it (fd dup — NO pixel copy), and samples the multi-plane NV12
# EGLImage (Mesa does YUV->RGB in the sampler). Recovering the clip's color proves decode->texture is
# copy-free. SKIPs (exit 77) without ffmpeg, a VAAPI device, or a GL render node.
import os
import shutil
import subprocess
import sys
import tempfile

SKIP = 77


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: run_video_decode_test.py <acl-video-decode-probe>\n")
        return 2
    probe = sys.argv[1]
    if not os.access(probe, os.X_OK):
        sys.stdout.write("SKIP: probe not built\n")
        return SKIP
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        sys.stdout.write("SKIP: ffmpeg CLI not found (fixture clip generator)\n")
        return SKIP

    work = tempfile.mkdtemp(prefix="acl-vdec-")
    try:
        clip = os.path.join(work, "solid.mp4")
        # A solid mid-green clip, yuv420p H.264 — a known color to recover after the round trip.
        r, g, b = 64, 160, 96
        gen = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
             "-i", f"color=c=0x{r:02X}{g:02X}{b:02X}:s=320x240:d=1:r=10",
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "main", clip],
            capture_output=True, text=True)
        if gen.returncode != 0 or not os.path.isfile(clip):
            sys.stdout.write(f"SKIP: ffmpeg could not encode the fixture ({gen.stderr.strip()[:200]})\n")
            return SKIP
        p = subprocess.run([probe, clip, f"{r},{g},{b}"], capture_output=True, text=True, timeout=120)
        out = p.stdout + p.stderr
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if p.returncode == SKIP or p.returncode == 2:
        sys.stdout.write("SKIP: no VAAPI/GL for zero-copy decode\n" + out)
        return SKIP
    if p.returncode != 0:
        sys.stderr.write("FAIL: video-decode probe\n" + out)
        return 1
    sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

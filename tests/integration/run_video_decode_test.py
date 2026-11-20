# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; FFmpeg CLI (lavfi color source) for the fixture clip; the
# zero-copy video-decode probe (src/media/video_decode_probe_main.cpp). No GPL-3.0 sources.
#
# The zero-copy hardware-decode path end-to-end ([[flutter-texture-bridge]] M5): generate a
# solid-color H.264 clip, then acl-video-decode-probe VAAPI-decodes it, maps each frame to a
# DRM_PRIME dma-buf, ahb_imports it (fd dup — NO pixel copy), and samples the multi-plane NV12
# EGLImage (Mesa does YUV->RGB in the sampler). Recovering the clip's color proves decode->texture is
# copy-free. SKIPs (exit 77) without ffmpeg, a VAAPI device, or a GL render node.
#
# With "10bit" as a second argument the clips are HEVC Main 10, which a hardware decoder
# gives as P010: 16-bit samples, rows twice as long. A frame described as NV12 there is
# refused by the GL import, which is how the FFmpeg engine failed until it was told apart.
# A test of its own, since it also needs an HEVC encoder and a decoder that does 10 bits.
# With no hardware decoder a 10-bit frame is copied into a P010 dma-buf.
#
# With "said" as the second argument the four-color clips are 8-bit HEVC that say how they
# were made, one BT.709 narrow range and one BT.601 full range, and must read back right
# through the converter: the GL import has to be told the matrix and the range, or the
# driver takes its own (Mesa: BT.601 narrow) and both come out wrong.
#
# The file decoder is always FFmpeg's. The packet runs go through the engine
# ACL_VIDEO_BACKEND names (dma, gst, ffmpeg), or the one a4l picks.
import os
import re
import shutil
import subprocess
import sys
import tempfile

SKIP = 77


def main() -> int:
    mode = sys.argv[2] if len(sys.argv) == 3 else ""
    if len(sys.argv) not in (2, 3) or mode not in ("", "10bit", "said"):
        sys.stderr.write("usage: run_video_decode_test.py <acl-video-decode-probe> "
                         "[10bit|said]\n")
        return 2
    ten = mode == "10bit"
    probe = sys.argv[1]
    # The encoder's arguments for a clip of each kind.
    x265 = ["-c:v", "libx265", "-x265-params", "log-level=error", "-tag:v", "hvc1"]
    codec = (x265 + ["-pix_fmt", "yuv420p10le"] if ten else
             x265 + ["-pix_fmt", "yuv420p"] if mode == "said" else
             ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "main"])
    # The four-color clips: a name, how the RGB is made YUV (empty: the encoder's own
    # way), and what the stream says of it.
    quads = ([("BT.709 narrow", "scale=out_color_matrix=bt709:out_range=tv",
               ["-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
                "-color_range", "tv"]),
              ("BT.601 full", "scale=out_color_matrix=bt601:out_range=pc",
               ["-colorspace", "smpte170m", "-color_range", "pc"])]
             if mode == "said" else [("", "", [])])
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
             "-i", f"color=c=0x{r:02X}{g:02X}{b:02X}:s=320x240:d=1:r=10"] + codec + [clip],
            capture_output=True, text=True)
        if gen.returncode != 0 or not os.path.isfile(clip):
            sys.stdout.write(f"SKIP: ffmpeg could not encode the fixture ({gen.stderr.strip()[:200]})\n")
            return SKIP
        # Both frame sources: file (VideoDecoder = demux+decode) and packet (Demuxer + CodecDecoder =
        # MediaExtractor + MediaCodec). Both must recover the color via the zero-copy DRM_PRIME import.
        runs = [("file", [probe, clip, f"{r},{g},{b}"]),
                ("packet", [probe, clip, f"{r},{g},{b}", "--packet"])]
        out = ""
        for label, cmd in runs:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            out += f"--- {label} ---\n" + p.stdout + p.stderr
            if p.returncode in (SKIP, 2):
                sys.stdout.write("SKIP: no VAAPI/GL for zero-copy decode\n" + out)
                return SKIP
            if p.returncode != 0:
                sys.stderr.write(f"FAIL: video-decode probe ({label})\n" + out)
                return 1

        # The converter (gfx::VideoConverter): what a compositor that does not take the
        # decoder's NV12 is sent. Four colors, one per quadrant, so that a flipped or
        # mirrored picture and a wrong color matrix both show.
        colors = {"tl": (0xC0, 0x20, 0x20), "tr": (0x20, 0x20, 0xC0),
                  "bl": (0x20, 0xC0, 0x20), "br": (0xE0, 0xE0, 0xE0)}
        for qname, made, says in quads:
            quad = os.path.join(work, "quad.mp4")
            inputs = []
            for name in ("tl", "tr", "bl", "br"):
                cr, cg, cb = colors[name]
                inputs += ["-f", "lavfi", "-i",
                           f"color=c=0x{cr:02X}{cg:02X}{cb:02X}:s=160x120:d=1:r=10" +
                           (",format=gbrp" if made else "")]
            gen = subprocess.run(
                [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + inputs +
                ["-filter_complex", "[0][1]hstack[t];[2][3]hstack[b];[t][b]vstack" +
                 (f",{made},format=yuv420p" if made else "")] + codec + says + [quad],
                capture_output=True, text=True)
            if gen.returncode != 0 or not os.path.isfile(quad):
                sys.stdout.write(f"SKIP: ffmpeg could not encode the quadrant clip "
                                 f"({gen.stderr.strip()[:200]})\n")
                return SKIP
            # From the file decoder and from the packet engine: the second is the only
            # run here in which an engine other than FFmpeg shows more than one color.
            for label, extra in (("convert", []), ("convert, packet", ["--packet"])):
                if qname:
                    label = f"{qname}, {label}"
                p = subprocess.run([probe, quad, "--convert"] + extra, capture_output=True,
                                   text=True, timeout=120)
                out += f"--- {label} ---\n" + p.stdout + p.stderr
                if p.returncode == 2:
                    sys.stdout.write("SKIP: the converted buffer cannot be read back here\n"
                                     + out)
                    return SKIP
                if p.returncode != 0:
                    sys.stderr.write(f"FAIL: video-decode probe ({label})\n" + out)
                    return 1
                if "fourcc=0x34324258 mod=0 " not in p.stdout:
                    sys.stderr.write("FAIL: the converted frame is not XBGR8888 LINEAR\n" + out)
                    return 1
                if ten and "fourcc=0x30313050 " not in p.stdout:
                    sys.stderr.write("FAIL: the 10-bit frame was not decoded as P010\n" + out)
                    return 1
                for name, want in colors.items():
                    m = re.search(rf"quadrant {name} = (\d+),(\d+),(\d+)", p.stdout)
                    got = tuple(int(v) for v in m.groups()) if m else None
                    if got is None or any(abs(a - b) > 12 for a, b in zip(got, want)):
                        sys.stderr.write(f"FAIL: {label}: quadrant {name} is {got}, "
                                         f"want {want}\n" + out)
                        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)

    sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

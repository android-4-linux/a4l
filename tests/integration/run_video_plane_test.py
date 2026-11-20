# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; FFmpeg CLI for the clips; the video plane probe
# (src/media/video_plane_probe_main.cpp). No GPL-3.0 sources.
#
# A hardware-decoded picture on a compositor's video plane, read back off the screen.
#
# The decode tests sample a frame through GL or the converter. What an app's video shows
# goes another way: the decoder's dma-buf is handed to the compositor as it is, on a
# subsurface, and the compositor does the YUV to RGB conversion. This test puts a clip of
# four colors, one per quadrant, on that plane with acl-video-plane-probe, takes a
# screenshot, and reads the four colors back. A wrong format, plane offset or row length
# shows as wrong colors or colors in the wrong place.
#
# Four runs: 8-bit (NV12) and 10-bit (P010) HEVC, each straight to the compositor and
# across the --split boundary, where the broker must also take every frame and refuse
# none.
#
# Needs: sway (started headless, for itself) and grim for the screenshot, ffmpeg with an
# HEVC encoder, and a hardware HEVC decoder that media::DmaCodec reaches. Exits 77 (skip)
# where any of these is missing. A 10-bit run on a decoder that does 8 bits only is a
# skip too.
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SKIP = 77
COLORS = {"tl": (0xC0, 0x20, 0x20), "tr": (0x20, 0x20, 0xC0),
          "bl": (0x20, 0xC0, 0x20), "br": (0xE0, 0xE0, 0xE0)}
W, H = 640, 360
# The center of each quadrant of a picture at the output's top left, where sway puts the
# one window there is.
POINTS = {"tl": (W // 4, H // 4), "tr": (3 * W // 4, H // 4),
          "bl": (W // 4, 3 * H // 4), "br": (3 * W // 4, 3 * H // 4)}
TOLERANCE = 12


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def listening(path):
    """Whether something accepts a connection on the socket at `path`."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
        return True
    except OSError:
        return False
    finally:
        s.close()


def start_sway(runtime_dir, work):
    """A headless sway with one 1280x800 output. Returns (process, wayland socket) or
    (None, reason)."""
    sway = shutil.which("sway")
    if not sway:
        return None, "sway is not installed"
    conf = os.path.join(work, "sway.conf")
    with open(conf, "w") as fh:
        fh.write("output HEADLESS-1 resolution 1280x800\ndefault_border none\n")
    # Sway's socket is the one that is listening now and was not before: a compositor
    # that was killed leaves its socket file behind, and sway takes that name again.
    before = {p for p in glob.glob(os.path.join(runtime_dir, "wayland-*"))
              if not p.endswith(".lock") and listening(p)}
    env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "DISPLAY")}
    env.update(WLR_BACKENDS="headless", WLR_LIBINPUT_NO_DEVICES="1",
               SWAYSOCK=os.path.join(work, "sway.sock"))
    with open(os.path.join(work, "sway.log"), "wb") as log:
        proc = subprocess.Popen([sway, "-c", conf], env=env, stdout=log, stderr=log,
                                start_new_session=True)
    for _ in range(50):
        new = [p for p in glob.glob(os.path.join(runtime_dir, "wayland-*"))
               if p not in before and not p.endswith(".lock") and listening(p)]
        if new:
            return proc, os.path.basename(new[0])
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    proc.kill()
    return None, "no headless sway could be started"


def screenshot(grim, display, path):
    """The output as a function from (x, y) to (r, g, b), or None."""
    r = subprocess.run([grim, "-t", "ppm", path], env=dict(os.environ, WAYLAND_DISPLAY=display),
                       capture_output=True)
    if r.returncode != 0 or not os.path.isfile(path):
        return None
    with open(path, "rb") as fh:
        data = fh.read()
    # P6, width, height, maxval, then one whitespace byte and the pixels.
    parts = data.split(None, 4)
    if len(parts) < 5 or parts[0] != b"P6":
        return None
    w, h = int(parts[1]), int(parts[2])
    px = parts[4]
    if len(px) < w * h * 3 or w < W or h < H:
        return None
    return lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: run_video_plane_test.py <acl-video-plane-probe>\n")
        return 2
    probe = sys.argv[1]
    if not os.access(probe, os.X_OK):
        return skip("probe not built")
    ffmpeg, grim = shutil.which("ffmpeg"), shutil.which("grim")
    if ffmpeg is None:
        return skip("ffmpeg CLI not found")
    if grim is None:
        return skip("grim is not installed")
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not runtime_dir or not os.path.isdir(runtime_dir):
        return skip("XDG_RUNTIME_DIR not set")

    work = tempfile.mkdtemp(prefix="a4l-vplane-")
    sway = None
    try:
        # The clips: four colors, Annex B with an access unit delimiter before each
        # frame, which is what the probe splits on, and no B frames.
        clips = {}
        for depth, pix in ((8, "yuv420p"), (10, "yuv420p10le")):
            inputs = []
            for name in ("tl", "tr", "bl", "br"):
                r, g, b = COLORS[name]
                inputs += ["-f", "lavfi", "-i",
                           f"color=c=0x{r:02X}{g:02X}{b:02X}:s={W // 2}x{H // 2}:r=30"]
            clip = os.path.join(work, f"quad{depth}.h265")
            gen = subprocess.run(
                [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + inputs +
                ["-filter_complex", "[0][1]hstack[t];[2][3]hstack[b];[t][b]vstack",
                 "-frames:v", "60", "-c:v", "libx265", "-x265-params",
                 "bframes=0:log-level=error", "-pix_fmt", pix,
                 "-bsf:v", "hevc_metadata=aud=insert", "-f", "hevc", clip],
                capture_output=True, text=True)
            if gen.returncode != 0 or not os.path.isfile(clip):
                return skip(f"ffmpeg could not encode the {depth}-bit clip",
                            gen.stderr.strip()[:300] + "\n")
            clips[depth] = clip

        sway, display = start_sway(runtime_dir, work)
        if sway is None:
            return skip(display)
        env = dict(os.environ, WAYLAND_DISPLAY=display)

        summary = []
        for depth, fourcc in ((8, "0x3231564e"), (10, "0x30313050")):
            for split in (False, True):
                tag = f"{depth}-bit" + (", --split" if split else "")
                cmd = [probe, clips[depth], "--size", f"{W}x{H}", "--loops", "2"]
                p = subprocess.Popen(cmd + (["--split"] if split else []), env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, errors="replace")
                # The picture is up once the probe has said what its frames are; half a
                # second more for the compositor to have drawn one.
                lines, at, deadline = [], None, time.time() + 60
                for line in p.stdout:
                    lines.append(line)
                    if "video-plane-probe: frames" in line:
                        time.sleep(0.5)
                        at = screenshot(grim, display, os.path.join(work, "shot.ppm"))
                        break
                    if time.time() > deadline:
                        break
                rest, _ = p.communicate(timeout=60)
                out = "".join(lines) + (rest or "")
                if p.returncode == 2:
                    return skip(f"{tag}: no decoder, clip or display for the probe", out[-2000:])

                def fail(why):
                    sys.stderr.write(f"FAIL ({tag}): {why}\n{out[-4000:]}\n")
                    return 1

                if p.returncode != 0:
                    return fail(f"the probe exited {p.returncode}")
                if f"format {fourcc} " not in out:
                    return fail(f"the frames are not format {fourcc}")
                if split:
                    m = re.search(r"broker took (\d+) video frame\(s\), refused (\d+)", out)
                    if not m or int(m.group(1)) == 0 or int(m.group(2)) != 0:
                        return fail("the broker did not take every frame")
                if at is None:
                    return fail("no screenshot was taken while the clip played")
                got = {name: at(*POINTS[name]) for name in COLORS}
                for name, want in COLORS.items():
                    if any(abs(a - b) > TOLERANCE for a, b in zip(got[name], want)):
                        return fail(f"quadrant {name} on the screen is {got[name]}, "
                                    f"want {want}")
                summary.append(f"{tag}: " + " ".join(
                    "%d,%d,%d" % got[name] for name in ("tl", "tr", "bl", "br")))
        sys.stdout.write("OK: " + "; ".join(summary) + "\n")
        return 0
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: timed out\n")
        return 1
    finally:
        if sway is not None:
            sway.terminate()
            try:
                sway.wait(timeout=5)
            except subprocess.TimeoutExpired:
                sway.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

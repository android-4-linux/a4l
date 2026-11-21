# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; FFmpeg CLI for the clips; the video plane probe
# (src/media/video_plane_probe_main.cpp); the PNG format (RFC 2083) for reading Weston's
# screenshot. No GPL-3.0 sources.
#
# A hardware-decoded picture on a compositor's video plane, read back off the screen.
#
# The decode tests sample a frame through GL or the converter. What an app's video shows
# goes another way: the decoder's dma-buf is handed to the compositor as it is, on a
# subsurface, and the compositor does the YUV to RGB conversion. This test puts a clip of
# four colors, one per quadrant, on that plane with acl-video-plane-probe, takes a
# screenshot, and reads the four colors back. A wrong format, plane offset or row length
# shows as wrong colors or colors in the wrong place, and a wrong conversion matrix as
# colors that are off: green by 25 of 255 between BT.601 and BT.709.
#
# Under each compositor it can start, headless: sway (screenshot by grim) and Weston
# (weston-screenshooter). They differ in what they convert with when nobody says: for a
# 640x360 frame sway takes BT.601 and Weston 15 BT.709. Weston has wp_color_representation
# and a4l tells it; sway 1.11 does not have it.
#
# The clips, each straight to the compositor and across the --split boundary, where the
# broker must also take every frame and refuse none: 8-bit (NV12) and 10-bit (P010) HEVC
# that does not say its matrix, made with BT.601, which is what a4l takes a picture of
# this size for.
#
# With "said" as a second argument the clips say how they were made, and only a compositor
# that can be told is of use: without the telling, or with what was said lost on the way
# across --split, they come out wrong. 8-bit made with BT.709, 10-bit (P010) made with
# BT.2020, and 8-bit made with BT.601 at full range, each saying so. BT.2020 here is the
# matrix only, with the SDR transfer: no HDR display is needed, and the compositor is not
# told the primaries, so it shows the RGB the clip was made from. A test of its own, so
# that where no such compositor is installed it shows as a skip.
#
# With "software" as a second argument the 8-bit and 10-bit clips are decoded by FFmpeg in
# software (--software), and each frame is copied into a dma-buf: NV12, and P010 with the
# ten bits moved to the top of each sample. No hardware decoder is needed.
#
# Needs: one of the two compositors with its screenshot tool, ffmpeg with an HEVC encoder,
# and a hardware HEVC decoder that media::DmaCodec reaches (in software mode, FFmpeg's
# HEVC decoder). Exits 77 (skip) where any of these is missing. A 10-bit run on a decoder
# that does 8 bits only is a skip too.
import glob
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

SKIP = 77
COLORS = {"tl": (0xC0, 0x20, 0x20), "tr": (0x20, 0x20, 0xC0),
          "bl": (0x20, 0xC0, 0x20), "br": (0xE0, 0xE0, 0xE0)}
W, H = 640, 360
TOLERANCE = 12
# BT.2020 and BT.709 are close: these colors made with BT.2020 and shown with BT.709 are
# off by at most 9, so the BT.2020 clip is held to less. What the compositors show is
# within 3.
TOLERANCE_BT2020 = 5


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


def read_ppm(path):
    """(width, height, RGB bytes) of a binary PPM, or None."""
    with open(path, "rb") as fh:
        data = fh.read()
    # P6, width, height, maxval, then one whitespace byte and the pixels.
    parts = data.split(None, 4)
    if len(parts) < 5 or parts[0] != b"P6":
        return None
    w, h = int(parts[1]), int(parts[2])
    return (w, h, parts[4]) if len(parts[4]) >= w * h * 3 else None


def read_png(path):
    """(width, height, RGB bytes) of an 8-bit RGB or RGBA PNG that is not interlaced, or
    None. Weston's screenshot is one."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, head = 8, b"", None
    while pos + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + size]
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + size
    if head is None or head[2] != 8 or head[3] not in (2, 6) or head[6] != 0:
        return None
    w, h, n = head[0], head[1], 3 if head[3] == 2 else 4
    raw = zlib.decompress(idat)
    row = w * n
    if len(raw) < (row + 1) * h:
        return None
    out, prev = bytearray(), bytearray(row)
    for y in range(h):
        kind = raw[y * (row + 1)]
        line = bytearray(raw[y * (row + 1) + 1:(y + 1) * (row + 1)])
        for i in range(row):
            a = line[i - n] if i >= n else 0
            b = prev[i]
            c = prev[i - n] if i >= n else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line if n == 3 else bytes(v for i, v in enumerate(line) if i % 4 != 3)
        prev = line
    return w, h, bytes(out)


def quadrants(image):
    """The colors at the centers of the picture's four quadrants, wherever the compositor
    put the window, or None with the reason. The picture is found by its three strong
    colors, loosely enough that a wrong matrix still finds it."""
    w, h, px = image

    def at(x, y):
        return tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])

    xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = at(x, y)
            if ((r > 150 and g < 90 and b < 90) or (b > 150 and r < 90 and g < 90) or
                    (g > 130 and r < 90 and b < 90)):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None, "no picture on the screen"
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    pw, ph = x1 - x0 + 2, y1 - y0 + 2
    if abs(pw - W) > 8 or abs(ph - H) > 8:
        return None, f"the picture on the screen is about {pw}x{ph}, not {W}x{H}"
    return {"tl": at(x0 + pw // 4, y0 + ph // 4), "tr": at(x0 + 3 * pw // 4, y0 + ph // 4),
            "bl": at(x0 + pw // 4, y0 + 3 * ph // 4),
            "br": at(x0 + 3 * pw // 4, y0 + 3 * ph // 4)}, None


def weston_tools():
    """(weston, weston-screenshooter) from one directory, the system's before PATH's: a
    second install elsewhere may be of another version, or not run at all."""
    found = shutil.which("weston")
    for d in ("/usr/bin", os.path.dirname(found) if found else None):
        if not d:
            continue
        pair = os.path.join(d, "weston"), os.path.join(d, "weston-screenshooter")
        if all(os.access(t, os.X_OK) for t in pair):
            return pair
    return None


def start_weston(runtime_dir, work):
    """A headless Weston that allows screenshots. Returns (process, socket name) or
    (None, reason)."""
    name = f"a4l-vplane-test-{os.getpid()}"
    for exe in (weston_tools()[0],):
        env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
        with open(os.path.join(work, "weston.log"), "wb") as log:
            proc = subprocess.Popen(
                [exe, "--backend=headless", "--renderer=gl", "--width=1280", "--height=800",
                 f"--socket={name}", "--idle-time=0", "--debug"],
                env=env, stdout=log, stderr=log, start_new_session=True)
        for _ in range(50):
            if os.path.exists(os.path.join(runtime_dir, name)):
                return proc, name
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        proc.kill()
    return None, "no headless Weston could be started"


def shot_grim(display, work):
    path = os.path.join(work, "shot.ppm")
    r = subprocess.run([shutil.which("grim"), "-t", "ppm", path],
                       env=dict(os.environ, WAYLAND_DISPLAY=display), capture_output=True)
    return read_ppm(path) if r.returncode == 0 and os.path.isfile(path) else None


def shot_weston(display, work):
    # Writes wayland-screenshot-<date>.png into the directory it runs in.
    for old in glob.glob(os.path.join(work, "wayland-screenshot-*.png")):
        os.unlink(old)
    r = subprocess.run([weston_tools()[1]], cwd=work,
                       env=dict(os.environ, WAYLAND_DISPLAY=display), capture_output=True)
    made = glob.glob(os.path.join(work, "wayland-screenshot-*.png"))
    return read_png(made[0]) if r.returncode == 0 and made else None


def main() -> int:
    mode = sys.argv[2] if len(sys.argv) == 3 else ""
    said = mode == "said"
    software = mode == "software"
    if len(sys.argv) not in (2, 3) or mode not in ("", "said", "software"):
        sys.stderr.write(
            "usage: run_video_plane_test.py <acl-video-plane-probe> [said|software]\n")
        return 2
    probe = sys.argv[1]
    if not os.access(probe, os.X_OK):
        return skip("probe not built")
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        return skip("ffmpeg CLI not found")
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not runtime_dir or not os.path.isdir(runtime_dir):
        return skip("XDG_RUNTIME_DIR not set")
    # (name, how to start it, how to take a screenshot), for each that is installed.
    compositors = []
    if shutil.which("sway") and shutil.which("grim"):
        compositors.append(("sway", start_sway, shot_grim))
    if weston_tools():
        compositors.append(("weston", start_weston, shot_weston))
    if not compositors:
        return skip("neither sway with grim nor weston with weston-screenshooter is installed")

    work = tempfile.mkdtemp(prefix="a4l-vplane-")
    running = None
    try:
        # The clips: four colors, Annex B with an access unit delimiter before each
        # frame, which is what the probe splits on, and no B frames. The colors are
        # made as RGB and converted with the matrix named, so that which one a clip was
        # made with is not ffmpeg's default.
        # name: (pixel format, matrix made with, range, whether the stream says so,
        #        DRM fourcc, what the compositor is to be told)
        kinds = ({"8-bit BT.709": ("yuv420p", "bt709", "tv", True, "0x3231564e",
                                   "BT.709 limited"),
                  "10-bit BT.2020": ("yuv420p10le", "bt2020", "tv", True, "0x30313050",
                                     "BT.2020 limited"),
                  "8-bit BT.601 full range": ("yuv420p", "bt601", "pc", True, "0x3231564e",
                                              "BT.601 full")} if said else
                 {"8-bit": ("yuv420p", "bt601", "tv", False, "0x3231564e", None),
                  "10-bit": ("yuv420p10le", "bt601", "tv", False, "0x30313050", None)})
        # What a clip says of itself: ffmpeg's names for the matrix and its primaries and
        # transfer (BT.2020's 10-bit transfer is BT.709's curve), and the range.
        says_tags = {"bt709": ("bt709", "bt709", "bt709"),
                     "bt2020": ("bt2020nc", "bt2020", "bt2020-10"),
                     "bt601": ("smpte170m", "smpte170m", "smpte170m")}
        clips = {}
        for kind, (pix, matrix, rng, says, _, _) in kinds.items():
            inputs = []
            for name in ("tl", "tr", "bl", "br"):
                r, g, b = COLORS[name]
                inputs += ["-f", "lavfi", "-i",
                           f"color=c=0x{r:02X}{g:02X}{b:02X}:s={W // 2}x{H // 2}:r=30,format=gbrp"]
            clip = os.path.join(work, f"clip{len(clips)}.h265")
            space, prim, trc = says_tags[matrix]
            tags = (["-colorspace", space, "-color_primaries", prim, "-color_trc", trc,
                     "-color_range", rng] if says else [])
            gen = subprocess.run(
                [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + inputs +
                ["-filter_complex",
                 "[0][1]hstack[t];[2][3]hstack[b];[t][b]vstack,"
                 f"scale=out_color_matrix={matrix}:out_range={rng},format={pix}",
                 "-frames:v", "60", "-c:v", "libx265", "-x265-params",
                 "bframes=0:log-level=error"] + tags +
                ["-bsf:v", "hevc_metadata=aud=insert", "-f", "hevc", clip],
                capture_output=True, text=True)
            if gen.returncode != 0 or not os.path.isfile(clip):
                return skip(f"ffmpeg could not encode the {kind} clip",
                            gen.stderr.strip()[:300] + "\n")
            clips[kind] = clip

        summary = []
        for comp, start, shot in compositors:
            running, display = start(runtime_dir, work)
            if running is None:
                return skip(display)
            env = dict(os.environ, WAYLAND_DISPLAY=display)
            time.sleep(1.5)  # Weston fades its desktop in, and a shot then is dim
            for kind, (_, _, _, says, fourcc, want_told) in kinds.items():
                for split in (False, True):
                    tag = f"{comp}, {kind}" + (", --split" if split else "")
                    cmd = [probe, clips[kind], "--size", f"{W}x{H}", "--loops", "2"]
                    if software:
                        cmd.append("--software")
                    p = subprocess.Popen(cmd + (["--split"] if split else []), env=env,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, errors="replace")
                    # The picture is up once the probe has said what its frames are;
                    # half a second more for the compositor to have drawn one.
                    lines, image, deadline = [], None, time.time() + 60
                    for line in p.stdout:
                        lines.append(line)
                        if "video-plane-probe: frames" in line:
                            time.sleep(0.5)
                            image = shot(display, work)
                            break
                        if time.time() > deadline:
                            break
                    rest, _ = p.communicate(timeout=60)
                    out = "".join(lines) + (rest or "")
                    if p.returncode == 2:
                        return skip(f"{tag}: no decoder, clip or display for the probe",
                                    out[-2000:])

                    def fail(why):
                        sys.stderr.write(f"FAIL ({tag}): {why}\n{out[-4000:]}\n")
                        return 1

                    if p.returncode != 0:
                        return fail(f"the probe exited {p.returncode}")
                    if f"format {fourcc} " not in out:
                        return fail(f"the frames are not format {fourcc}")
                    if software:
                        want = "P010" if fourcc == "0x30313050" else "NV12"
                        if not re.search(rf"software frames \d+x\d+: copied to {want}", out):
                            return fail(f"the frames were not decoded in software into {want}")
                    if split:
                        m = re.search(r"broker took (\d+) video frame\(s\), refused (\d+)", out)
                        if not m or int(m.group(1)) == 0 or int(m.group(2)) != 0:
                            return fail("the broker did not take every frame")
                    told = re.search(r"the compositor is told (BT\.\d+) (\w+) range", out)
                    if says:
                        # A compositor that cannot be told converts this clip its own
                        # way, which is not a4l's to get right.
                        if not told:
                            break  # this compositor is of no use here
                        if f"{told.group(1)} {told.group(2)}" != want_told:
                            return fail(f"the compositor was told {told.group(1)} "
                                        f"{told.group(2)}, and the stream says {want_told}")
                    if image is None:
                        return fail("no screenshot was taken while the clip played")
                    got, why = quadrants(image)
                    if got is None:
                        return fail(why)
                    tol = TOLERANCE_BT2020 if "BT.2020" in kind else TOLERANCE
                    for name, want in COLORS.items():
                        if any(abs(a - b) > tol for a, b in zip(got[name], want)):
                            return fail(f"quadrant {name} on the screen is {got[name]}, "
                                        f"want {want}")
                    summary.append(f"{tag}: " + " ".join(
                        "%d,%d,%d" % got[name] for name in ("tl", "tr", "bl", "br")) +
                        (f" (told {told.group(1)})" if told else ""))
            running.terminate()
            try:
                running.wait(timeout=5)
            except subprocess.TimeoutExpired:
                running.kill()
            running = None
        if not summary:
            return skip("no compositor here can be told a matrix (wp_color_representation)")
        sys.stdout.write("OK: " + "; ".join(summary) + "\n")
        return 0
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: timed out\n")
        return 1
    finally:
        if running is not None:
            running.terminate()
            try:
                running.wait(timeout=5)
            except subprocess.TimeoutExpired:
                running.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

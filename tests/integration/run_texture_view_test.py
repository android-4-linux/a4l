# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; FFmpeg CLI for the fixture clip;
# tests/fixtures/texture_view/Main.java. No GPL-3.0 sources.
#
# Video on a TextureView. A fixture app decodes an H.264 clip with MediaCodec onto
# new Surface(surfaceTexture), and the TextureView draws each frame inside the window. The clip
# is four colored quadrants, 640x360, so its buffer is 640x368 and the picture is cropped. The
# test reads back the last frame a4l rendered (ACL_GPU_DUMP), finds the TextureView in it, and
# checks each quadrant's color, near the middle and near the bottom edge, where a crop that was
# not applied would show the buffer's padding. It also checks that the app saw a frame update
# for most frames and that the SurfaceTexture's timestamp is the time the app released the
# frame for.
#
# Two runs: the frame imported on the present device (Vulkan) and the frame's pixels copied
# (ACL_SURFACETEXTURE_COPY), the path for a driver that refuses the import.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, ffmpeg with libx264, a decoder for
# H.264 (any of a4l's, software included), a render node, and a Wayland compositor. It starts a
# headless Weston on its own socket unless ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip)
# where any of these is missing.
#
# ACL_TEST_TEXTURE_KEEP=<dir> keeps the APK, the runs' logs and frames there.
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77
W, H = 640, 360          # Main.W x Main.H
FRAMES = 60              # 2 s at 30 fps
LOOPS = 2                # Main.LOOPS
# Top left, top right, bottom left, bottom right.
QUADRANTS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
TOLERANCE = 40           # per channel, for YUV 4:2:0 and back


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def start_weston(runtime_dir, log_path):
    """A headless Weston on a private socket. Returns (process, socket name) or (None, reason)."""
    name = f"a4l-texture-test-{os.getpid()}"
    for exe in ("/usr/bin/weston", shutil.which("weston")):
        if not exe or not os.access(exe, os.X_OK):
            continue
        env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                [exe, "--backend=headless", "--renderer=gl", f"--socket={name}", "--idle-time=0"],
                env=env, stdout=log, stderr=log, start_new_session=True)
        for _ in range(50):
            if os.path.exists(os.path.join(runtime_dir, name)):
                return proc, name
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        proc.kill()
    return None, "no headless Weston could be started"


def read_ppm(path):
    """(width, height, rgb bytes) of a binary PPM."""
    with open(path, "rb") as fh:
        data = fh.read()
    fields = []
    i = 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError(f"{path} is not an 8-bit binary PPM")
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i + 1:i + 1 + w * h * 3]


def near(a, b, tol=TOLERANCE):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def check_frame(path):
    """None if the TextureView shows the clip's quadrants, else what is wrong."""
    w, h, px = read_ppm(path)

    def at(x, y):
        o = (y * w + x) * 3
        return tuple(px[o:o + 3])

    # The TextureView is where the clip's colors are; the window has a bar of its own above
    # the app's background.
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            p = at(x, y)
            if any(near(p, q) for q in QUADRANTS):
                xs.append(x)
                ys.append(y)
    if not xs:
        return "the window shows none of the clip's colors"
    x0, y0, x1, y1 = min(xs), min(ys), max(xs) + 1, max(ys) + 1
    if (x1 - x0, y1 - y0) != (W, H):
        return f"the TextureView is {x1 - x0}x{y1 - y0} at ({x0},{y0}), not {W}x{H}"
    # The middle of each quadrant, and a point 2 rows above the picture's bottom edge in each
    # bottom quadrant: the buffer's 8 rows of padding are there when the crop is not applied.
    probes = [(W // 4, H // 4, 0), (3 * W // 4, H // 4, 1),
              (W // 4, 3 * H // 4, 2), (3 * W // 4, 3 * H // 4, 3),
              (W // 4, H - 3, 2), (3 * W // 4, H - 3, 3)]
    for x, y, q in probes:
        got = at(x0 + x, y0 + y)
        if not near(got, QUADRANTS[q]):
            return f"({x},{y}) in the TextureView is {got}, expected {QUADRANTS[q]}"
    return None


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_texture_view_test.py <a4l> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not runtime_dir or not os.path.isdir(runtime_dir):
        return skip("XDG_RUNTIME_DIR not set")
    if not os.path.isfile(android_jar):
        return skip("no staged android.jar")
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        return skip("ffmpeg CLI not found")
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    d8 = os.path.join(home, "bin", "d8")
    zipalign = first(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            return skip(f"{name} not found")

    def run(cmd, **kw):
        subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

    fixtures = os.path.join(src_root, "tests/fixtures")
    keep = os.environ.get("ACL_TEST_TEXTURE_KEEP", "")
    work = tempfile.mkdtemp(prefix="a4l-texture-")
    weston = None
    try:
        # 1. The clip: four solid quadrants, no B-frames, an access unit delimiter before each
        #    frame so the fixture can cut the stream into frames.
        clip = os.path.join(work, "clip.mp4")
        box = "drawbox=x={}:y={}:w=320:h=180:color={}:t=fill"
        g = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
             "-i", f"color=c=blue:s={W}x{H}:r=30:d=2," + ",".join(
                 [box.format(0, 0, "red"), box.format(320, 0, "lime"),
                  box.format(320, 180, "white")]),
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "main", "-bf", "0",
             "-g", "30", "-x264-params", "aud=1", clip],
            capture_output=True, text=True)
        if g.returncode != 0:
            return skip("ffmpeg could not encode the fixture clip", g.stderr)
        app = os.path.join(work, "app")
        os.makedirs(os.path.join(app, "assets"))
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", clip, "-c:v", "copy",
             "-bsf:v", "h264_mp4toannexb", "-f", "h264", os.path.join(app, "assets/clip.h264")])

        # 2. The fixture APK.
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls = os.path.join(work, "cls")
        os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "texture_view/Main.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.textureview",
                                launcher="com.acl.textureview.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "texture-view.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run(["zip", "-X", "-0", "-q", unaligned, "assets/clip.h264"], cwd=app)
        run([zipalign, "-f", "4", unaligned, apk])
        if keep:
            os.makedirs(keep, exist_ok=True)
            shutil.copy(apk, keep)

        # 3. A compositor.
        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        total = FRAMES * LOOPS
        notes = []
        for tag, vary in (("import", {}), ("copy", {"ACL_SURFACETEXTURE_COPY": "1"})):
            frame = os.path.join(work, f"frame-{tag}.ppm")
            env = dict(os.environ, **vary, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="90",
                       ACL_BIN=acl, ACL_PRESENT_GPU="1", ACL_GPU_DUMP=frame,
                       XDG_CACHE_HOME=os.path.join(work, f"cache-{tag}"),
                       XDG_DATA_HOME=os.path.join(work, f"data-{tag}"))
            p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                "--verify=off", "--present"],
                               env=env, capture_output=True, text=True, timeout=240)
            out = p.stdout + p.stderr
            if keep:
                with open(os.path.join(keep, f"run-{tag}.log"), "w") as fh:
                    fh.write(out)
                if os.path.exists(frame):
                    shutil.copy(frame, keep)

            def fail(why):
                sys.stderr.write(f"FAIL ({tag}): {why}\n{out[-6000:]}\n")
                return 1

            if p.returncode == SKIP:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            if "CodecDecoder::configure failed" in out:
                return skip("no H.264 decoder", out[-2000:])
            if "frame alpha:" not in out:
                return skip("this build does not report rendered frames (ACL_GPU_DUMP)",
                            out[-2000:])
            if "UnsatisfiedLinkError" in out:
                return fail("a native method is missing")
            m = re.search(r"TEXTURE: fed (\d+) rendered (\d+) updated (\d+) timestamp (\w+)", out)
            if not m:
                return fail("the fixture did not finish")
            rendered, updated = int(m.group(2)), int(m.group(3))
            if "frames go to a SurfaceTexture" not in out:
                return fail("the decoder's frames did not go to the SurfaceTexture")
            if rendered != total:
                return fail(f"the app rendered {rendered} of {total} frames")
            # The listener runs when the TextureView draws a new frame; a frame that arrives
            # while the last is not yet drawn replaces it, so a few are not seen.
            if updated < total * 8 // 10:
                return fail(f"the app saw {updated} frame updates for {total} frames")
            if m.group(4) != "matches":
                return fail("the SurfaceTexture's timestamp is not the time the app released "
                            "its last frame for")
            if not vary and "was not imported on the GPU" in out:
                return fail("the frame was copied, not imported")
            if not os.path.exists(frame):
                return fail("no rendered frame was read back")
            wrong = check_frame(frame)
            if wrong:
                return fail(wrong)
            notes.append(f"{tag}: {rendered} frames, {updated} updates, picture right")
        sys.stdout.write("OK: " + "; ".join(notes) + "\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired as e:
        out = e.stdout or ""
        if isinstance(out, bytes):
            out = out.decode(errors="replace")
        sys.stderr.write(f"FAIL: timed out\n{out[-4000:]}\n")
        return 1
    finally:
        if weston is not None:
            weston.terminate()
            try:
                weston.wait(timeout=5)
            except subprocess.TimeoutExpired:
                weston.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

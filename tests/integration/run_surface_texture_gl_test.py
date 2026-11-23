# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; FFmpeg CLI for the fixture clip;
# tests/fixtures/surface_texture_gl/Main.java. No GPL-3.0 sources.
#
# Video sampled in the app's own GL. A fixture app decodes a clip with MediaCodec onto
# new Surface(new SurfaceTexture(tex)); on each frame its GL thread calls updateTexImage and draws
# the external texture through getTransformMatrix at the picture's size, then reads back points
# in each quadrant, at the picture's corners and either side of the edges between quadrants.
# The clip is 640x360 inside a 16- and 12-pixel magenta border that its stream crops away (frame
# cropping on all four sides), so a crop not applied, or not carried by the matrix, shows.
# Every frame the app saw must sample right, the matrix must flip Y, and the timestamp must be
# the time the app released its last frame for. Runs with no display server, so EGL14 is on
# Mesa's surfaceless platform.
#
# Two runs: the decoder's frame bound to the app's texture as it is, and converted to XBGR8888
# first (ACL_SURFACETEXTURE_CONVERT), the path for a driver that refuses the decoder's frame.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, ffmpeg with libx264 (libx265 for
# HEVC), a decoder for the codec (any of a4l's, software included) and a render node. Exits 77
# (skip) where any of these is missing.
#
# ACL_TEST_VIDEO_CODEC picks the clip's codec: h264 (the default), hevc, or hevc10 (HEVC Main 10).
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

SKIP = 77
W, H = 640, 360          # Main.W x Main.H
FRAMES = 60              # 2 s at 30 fps
LOOPS = 2                # Main.LOOPS


# The clip's codec (ACL_TEST_VIDEO_CODEC): ffmpeg's encoder arguments, the stream's MIME type, the
# raw format to write and the bitstream filter that sets its cropping.
CODECS = {
    "h264": (["-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "main", "-bf", "0",
              "-g", "30", "-x264-params", "aud=1"], "video/avc", "h264", "h264_metadata"),
    "hevc": (["-c:v", "libx265", "-pix_fmt", "yuv420p", "-profile:v", "main",
              "-x265-params", "aud=1:bframes=0:keyint=30:log-level=error"],
             "video/hevc", "hevc", "hevc_metadata"),
    "hevc10": (["-c:v", "libx265", "-pix_fmt", "yuv420p10le", "-profile:v", "main10",
                "-x265-params", "aud=1:bframes=0:keyint=30:log-level=error"],
               "video/hevc", "hevc", "hevc_metadata"),
}


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_surface_texture_gl_test.py <a4l> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    codec = os.environ.get("ACL_TEST_VIDEO_CODEC", "h264")
    if codec not in CODECS:
        sys.stderr.write(f"ACL_TEST_VIDEO_CODEC={codec}: one of {', '.join(CODECS)}\n")
        return 2
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    if not os.path.isfile(android_jar):
        return skip("no staged android.jar")
    if not glob.glob("/dev/dri/renderD*"):
        return skip("no render node")
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
    work = tempfile.mkdtemp(prefix="a4l-stgl-")
    try:
        # The clip: four solid quadrants in a magenta border the stream crops away, no
        # B-frames, an access unit delimiter before each frame.
        enc, mime, raw, bsf = CODECS[codec]
        app = os.path.join(work, "app")
        os.makedirs(os.path.join(app, "assets"))
        cx, cy = 16, 12
        box = "drawbox=x={}:y={}:w=320:h=180:color={}:t=fill"
        g = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
             "-i", f"color=c=magenta:s={W + 2 * cx}x{H + 2 * cy}:r=30:d=2," + ",".join(
                 [box.format(cx, cy, "red"), box.format(cx + 320, cy, "lime"),
                  box.format(cx, cy + 180, "blue"), box.format(cx + 320, cy + 180, "white")])]
            + enc + ["-bsf:v", f"{bsf}=crop_left={cx}:crop_right={cx}:crop_top={cy}:"
                               f"crop_bottom={cy}",
                     "-f", raw, os.path.join(app, "assets/clip")],
            capture_output=True, text=True)
        if g.returncode != 0:
            return skip(f"ffmpeg could not encode the fixture clip as {codec}", g.stderr)
        with open(os.path.join(app, "assets/mime"), "w") as fh:
            fh.write(mime)

        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        os.makedirs(cls)
        os.makedirs(dexdir)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "surface_texture_gl/Main.java")])
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.stgl",
                                launcher="com.acl.stgl.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "surface-texture-gl.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run(["zip", "-X", "-0", "-q", unaligned, "assets/clip", "assets/mime"], cwd=app)
        run([zipalign, "-f", "4", unaligned, apk])
        keep = os.environ.get("ACL_TEST_STGL_KEEP", "")
        if keep:
            os.makedirs(keep, exist_ok=True)
            shutil.copy(apk, keep)

        base = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        base.update(ACL_RUN_TIMEOUT="90", ACL_BIN=acl,
                    XDG_CACHE_HOME=os.path.join(work, "cache"),
                    XDG_DATA_HOME=os.path.join(work, "data"))
        total = FRAMES * LOOPS
        notes = []
        for tag, vary, said in (
                ("as it is", {}, "frames go to a SurfaceTexture as they are"),
                ("converted", {"ACL_SURFACETEXTURE_CONVERT": "1"}, "converted to XBGR8888")):
            p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off"],
                               env=dict(base, **vary), capture_output=True, text=True,
                               timeout=240)
            out = p.stdout + p.stderr
            if keep:
                with open(os.path.join(keep, f"run-{tag.replace(' ', '-')}.log"), "w") as fh:
                    fh.write(out)

            def fail(why):
                sys.stderr.write(f"FAIL ({tag}): {why}\n{out[-6000:]}\n")
                return 1

            if p.returncode == SKIP:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            if "CodecDecoder::configure failed" in out:
                return skip(f"no {codec} decoder", out[-2000:])
            if "EGL14 natives registered" not in out:
                return skip("this build has no EGL14 natives", out[-2000:])
            m = re.search(r"STGL: rendered (\d+) updated (\d+) right (\d+) matrix (\S+) "
                          r"timestamp (\w+) last (.*)", out)
            if not m:
                return fail("the fixture did not finish")
            rendered, updated, right = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if said not in out:
                return fail(f"the frames did not go to the app's GL {tag}")
            if rendered != total:
                return fail(f"the app rendered {rendered} of {total} frames")
            if updated < total * 8 // 10:
                return fail(f"the app saw {updated} frames of {total}")
            if right != updated:
                return fail(f"{updated - right} of {updated} frames sampled wrong; the last read "
                            f"{m.group(6)}")
            sx, sy, tx, ty = (float(v) for v in m.group(4).split(","))
            if sy >= 0:
                return fail(f"getTransformMatrix does not flip Y: {m.group(4)}")
            if m.group(5) != "matches":
                return fail("the SurfaceTexture's timestamp is not the time the app released "
                            "its last frame for")
            notes.append(f"{tag}: {updated} of {rendered} frames drawn by the app, all sampled "
                         f"right, matrix scale {sx:.4f},{sy:.4f} offset {tx:.4f},{ty:.4f}")
        sys.stdout.write("OK: " + "; ".join(notes) + "\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: timed out\n")
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

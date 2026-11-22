# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; FFmpeg CLI for the fixture clip;
# tests/fixtures/surface_texture_gl/Main.java. No GPL-3.0 sources.
#
# Video sampled in the app's own GL. A fixture app decodes an H.264 clip with MediaCodec onto
# new Surface(new SurfaceTexture(tex)); on each frame its GL thread calls updateTexImage and draws
# the external texture through getTransformMatrix, then reads back the four quadrants of the
# clip and two points near its bottom edge (the clip is 640x360 in a 640x368 buffer). Every frame
# the app saw must sample right, the matrix must be the Y flip, and the timestamp must be the
# time the app released its last frame for. Runs with no display server, so EGL14 is on Mesa's
# surfaceless platform.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, ffmpeg with libx264, a decoder for
# H.264 (any of a4l's, software included) and a render node. Exits 77 (skip) where any of these
# is missing.
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
        # The clip: four solid quadrants, no B-frames, an access unit delimiter before each frame.
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
        run(["zip", "-X", "-0", "-q", unaligned, "assets/clip.h264"], cwd=app)
        run([zipalign, "-f", "4", unaligned, apk])
        keep = os.environ.get("ACL_TEST_STGL_KEEP", "")
        if keep:
            os.makedirs(keep, exist_ok=True)
            shutil.copy(apk, keep)

        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update(ACL_RUN_TIMEOUT="90", ACL_BIN=acl,
                   XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off"],
                           env=env, capture_output=True, text=True, timeout=240)
        out = p.stdout + p.stderr
        if keep:
            with open(os.path.join(keep, "run.log"), "w") as fh:
                fh.write(out)

        def fail(why):
            sys.stderr.write(f"FAIL: {why}\n{out[-6000:]}\n")
            return 1

        if p.returncode == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        if "CodecDecoder::configure failed" in out:
            return skip("no H.264 decoder", out[-2000:])
        if "EGL14 natives registered" not in out:
            return skip("this build has no EGL14 natives", out[-2000:])
        m = re.search(r"STGL: rendered (\d+) updated (\d+) right (\d+) matrix (\w+) "
                      r"timestamp (\w+) last (.*)", out)
        if not m:
            return fail("the fixture did not finish")
        rendered, updated, right = int(m.group(1)), int(m.group(2)), int(m.group(3))
        total = FRAMES * LOOPS
        if "frames go to a SurfaceTexture" not in out:
            return fail("the decoder's frames did not go to the SurfaceTexture")
        if rendered != total:
            return fail(f"the app rendered {rendered} of {total} frames")
        if updated < total * 8 // 10:
            return fail(f"the app saw {updated} frames of {total}")
        if right != updated:
            return fail(f"{updated - right} of {updated} frames sampled wrong; the last read "
                        f"{m.group(6)}")
        if m.group(4) != "flip":
            return fail(f"getTransformMatrix gave {m.group(4)}, not the Y flip")
        if m.group(5) != "matches":
            return fail("the SurfaceTexture's timestamp is not the time the app released its "
                        "last frame for")
        sys.stdout.write(f"OK: {rendered} frames, {updated} drawn by the app, all sampled "
                         f"right ({m.group(6)})\n")
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

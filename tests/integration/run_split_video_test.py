# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness (docs/adr/ADR-0009 C3, video); FFmpeg CLI for the fixture
# clip; tests/fixtures/video_plane/Main.java. No GPL-3.0 sources.
#
# Video across the --split boundary. A fixture app decodes an H.264 clip with MediaCodec onto a
# SurfaceView. Under `a4l run --split` the decoder runs in the confined app process and each
# decoded frame is sent to the broker, which checks it and puts it on the video subsurface. The
# test asserts that the frames the app rendered reached the broker and that none was refused.
#
# It first runs the app single-process and reads back the window a4l rendered: the SurfaceView's
# rectangle must be transparent and the rest opaque. The video is on a surface below the window,
# so it shows only through that hole, and the app has an opaque background for the hole to go
# through.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, ffmpeg, a VA-API decoder, and a
# Wayland compositor. It starts a headless Weston on its own socket unless
# ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip) where any of these is missing.
#
# The app is run in two forms: with the SurfaceView in the window at launch, and with it added
# after the first frames.
#
# ACL_TEST_VIDEO_KEEP=<dir> keeps the APKs and the runs' logs there.
# ACL_TEST_VIDEO_ARGS="..." replaces the a4l arguments (default: --split --present) and
#   leaves out the hole check.
# ACL_TEST_VIDEO_VARIANT=launch|late runs one form.
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77
FRAMES_PER_LOOP = 60   # 2 s at 30 fps
LOOPS = 3              # Main.LOOPS


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
    name = f"a4l-video-test-{os.getpid()}"
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


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_split_video_test.py <a4l> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    if not android_jar or not os.path.isfile(android_jar):
        return skip("no staged android.jar")
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        return skip("ffmpeg CLI not found")
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not runtime_dir or not os.path.isdir(runtime_dir):
        return skip("XDG_RUNTIME_DIR not set")
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
    sys.path.insert(0, fixtures)
    from make_axml import build_axml  # noqa: E402

    keep = os.environ.get("ACL_TEST_VIDEO_KEEP", "")
    work = tempfile.mkdtemp(prefix="a4l-video-")
    weston = None
    try:
        # 1. A moving test pattern, 640x360, 2 s at 30 fps, no B-frames, an access unit
        #    delimiter before each frame so the fixture can cut the stream into frames.
        clip = os.path.join(work, "clip.mp4")
        g = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
             "-i", "testsrc2=s=640x360:r=30:d=2", "-c:v", "libx264", "-pix_fmt", "yuv420p",
             "-profile:v", "main", "-bf", "0", "-g", "30", "-x264-params", "aud=1", clip],
            capture_output=True, text=True)
        if g.returncode != 0:
            return skip("ffmpeg could not encode the fixture clip", g.stderr)
        app = os.path.join(work, "app")
        os.makedirs(os.path.join(app, "assets"))
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", clip, "-c:v", "copy",
             "-bsf:v", "h264_mp4toannexb", "-f", "h264", os.path.join(app, "assets/clip.h264")])

        # 2. The fixture, as two APKs off one dex: the SurfaceView in the window at launch
        #    (Main) and added after the first frames (MainLate). a4l decides at launch whether
        #    a SurfaceView is a GL surface or a video surface, so both are run.
        cls = os.path.join(work, "cls")
        os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "video_plane/Main.java"),
             os.path.join(fixtures, "video_plane/MainLate.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])

        def build_apk(launcher, tag):
            with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
                fh.write(build_axml(utf8=True, package="com.acl.videoplane", launcher=launcher))
            unaligned = os.path.join(work, f"u-{tag}.apk")
            apk = os.path.join(work, f"video-plane-{tag}.apk")
            run(["zip", "-X", "-0", "-j", "-q", unaligned,
                 os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
            run(["zip", "-X", "-0", "-q", unaligned, "assets/clip.h264"], cwd=app)
            run([zipalign, "-f", "4", unaligned, apk])
            return apk

        variants = [("launch", build_apk("com.acl.videoplane.Main", "launch")),
                    ("late", build_apk("com.acl.videoplane.MainLate", "late"))]
        only = os.environ.get("ACL_TEST_VIDEO_VARIANT", "")
        if only:
            variants = [v for v in variants if v[0] == only]

        # 3. A compositor.
        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        total = FRAMES_PER_LOOP * LOOPS
        want_hole = 640 * 360  # Main.W x Main.H; nothing is drawn over the video
        split_args = os.environ.get("ACL_TEST_VIDEO_ARGS", "--split --present").split()
        check_hole = "ACL_TEST_VIDEO_ARGS" not in os.environ
        notes = []

        for tag, apk in variants:
            env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="90",
                       ACL_BIN=acl, ACL_PRESENT_GPU="1",
                       XDG_CACHE_HOME=os.path.join(work, f"cache-{tag}"),
                       XDG_DATA_HOME=os.path.join(work, f"data-{tag}"))
            runner = [os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off"]

            def launch(args, extra_env=None, label=""):
                p = subprocess.run(runner + args, env=dict(env, **(extra_env or {})),
                                   capture_output=True, text=True, timeout=240)
                out = p.stdout + p.stderr
                if keep:
                    os.makedirs(keep, exist_ok=True)
                    shutil.copy(apk, keep)
                    with open(os.path.join(keep, f"run-{tag}{label}.log"), "w") as fh:
                        fh.write(out)
                return p.returncode, out

            def skip_reason(rc, out):
                if rc == SKIP:
                    return "run-apk.sh (vendored framework not built)"
                if "confinement is not available" in out or "cannot confine" in out:
                    return "the app process cannot be confined here"
                if "SKIP: no VAAPI" in out or "CodecDecoder::configure failed" in out:
                    return "no VA-API decoder"
                if "needs the GPU present path" in out:
                    return "built without the GPU present path"
                return None

            def fail(why, out):
                sys.stderr.write(f"FAIL ({tag}): {why}\n{out[-6000:]}\n")
                return 1

            # 4. The hole. ACL_GPU_DUMP makes a4l read back each frame it renders and report
            #    how many pixels are opaque and how many clear.
            if check_hole:
                rc, out = launch(["--present"],
                                 {"ACL_GPU_DUMP": os.path.join(work, f"frame-{tag}.ppm")}, "-hole")
                why = skip_reason(rc, out)
                if why:
                    return skip(why, out[-2000:])
                if "-> GL present" in out:
                    return fail("the SurfaceView was taken for a GL surface", out)
                alpha = [tuple(int(v) for v in m) for m in re.findall(
                    r"frame alpha: opaque=(\d+) clear=(\d+) partial=(\d+) \((\d+)x(\d+)\)",
                    out)]
                if not alpha:
                    return skip("this build does not report rendered frames (ACL_GPU_DUMP)",
                                out[-2000:])
                if "maybeInjectVideoSurface: video SurfaceView surfaceCreated" not in out:
                    return fail("the SurfaceView never got a video surface", out)
                opaque, clear, partial, w, h = alpha[-1]
                if clear != want_hole or partial != 0 or opaque != w * h - want_hole:
                    return fail(
                        f"the window should be opaque but for a {want_hole}-pixel hole at the "
                        f"SurfaceView; the last frame has opaque={opaque} clear={clear} "
                        f"partial={partial} of {w}x{h}", out)
                if tag == "late" and alpha[0][1] != 0:
                    return fail(f"the window had clear pixels before the SurfaceView joined "
                                f"it: {alpha[0]}", out)

            # 5. Across the boundary.
            rc, out = launch(split_args)
            why = skip_reason(rc, out)
            if why:
                return skip(why, out[-2000:])
            if "-> GL present" in out:
                return fail("the SurfaceView was taken for a GL surface", out)
            m = re.search(r"VIDEO: fed (\d+) rendered (\d+)", out)
            if not m:
                return fail("the fixture did not finish", out)
            fed, rendered = int(m.group(1)), int(m.group(2))
            if rendered < total * 9 // 10:
                return fail(f"the app rendered {rendered} of {total} frames", out)
            if "--split" not in split_args:
                notes.append(f"{tag}: single process, fed {fed}, rendered {rendered}")
                continue

            s = re.search(r"split: app process ended \((\w+)\); (\d+) frame\(s\) presented, "
                          r"(\d+) input event\(s\) forwarded, (\d+) message\(s\) rejected, .*?"
                          r"(\d+) video frame\(s\) shown", out)
            if not s:
                return fail("no broker summary", out)
            how, ui = s.group(1), int(s.group(2))
            rejected, video = int(s.group(4)), int(s.group(5))
            if how != "exited":
                return fail(f"the app process {how}", out)
            if rejected != 0:
                return fail(f"the broker rejected {rejected} message(s)", out)
            if "refused a video frame" in out:
                return fail("the broker refused a video frame", out)
            # Every frame the decoder produced was sent; the broker counts those it accepted.
            if video < total * 9 // 10:
                return fail(f"the broker showed {video} of {total} video frames", out)
            if ui < 1:
                return fail("the window itself never presented", out)
            notes.append(f"{tag}: {video} of {total} frames at the broker's video plane, "
                         f"{ui} window frame(s), 0 rejected")

        hole = (f"; a {want_hole}-pixel hole at the SurfaceView and the rest opaque, in both"
                if check_hole and len(variants) == 2 else "")
        sys.stdout.write("OK: " + "; ".join(notes) + hole + "\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired as e:
        sys.stderr.write(f"FAIL: timed out\n{(e.stdout or '')[-4000:]}\n")
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

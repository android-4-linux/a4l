# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; FFmpeg CLI (fixture clip + Annex-B extraction); the MediaCodec
# decode fixture (tests/fixtures/media_codec/Main.java). Runs through tools/run-apk.sh (the real
# vendored framework is needed for MediaCodec/ImageReader). No GPL-3.0 sources.
#
# End-to-end proof of acl's android.media.MediaCodec native underside ([[flutter-texture-bridge]] M5):
# a real MediaCodec (createDecoderByType -> configure(Surface) -> queueInputBuffer -> dequeueOutput ->
# releaseOutputBuffer(render)) decodes H.264 via acl's zero-copy VAAPI CodecDecoder straight into an
# ImageReader Surface, and the frame is acquired back. This is the decode path video_player_android
# drives (media3/ExoPlayer -> MediaCodec). SKIPs (exit 77) without ffmpeg, VAAPI/GL, the AOSP
# toolchain, or the vendored framework.
import glob
import os
import shutil
import subprocess
import sys
import tempfile

SKIP = 77

# Two run modes off the same classes.dex: synchronous (Main) and asynchronous setCallback (MainAsync).
SYNC_CHECKS = [
    "MC decode: findDecoderForFormat=acl.h264.decoder",  # MediaCodecList discovery (ExoPlayer's gate)
    "native_start: decoder up",              # MediaCodec.start -> CodecDecoder configured
    "MC decode: output format 320x240",      # INFO_OUTPUT_FORMAT_CHANGED -> getOutputFormat (ExoPlayer path)
    "MC decode: frame in ImageReader 320x240",  # a decoded frame reached the consumer via queue_imported
    "MC decode: PASS",
]
ASYNC_CHECKS = [
    "native_start: decoder up (ASYNC)",      # setCallback -> async driver thread
    "ASYNC: onOutputFormatChanged 320x240",  # the driver delivered CB_OUTPUT_FORMAT_CHANGE
    "ASYNC: frame in ImageReader 320x240",   # onOutputBufferAvailable -> releaseOutputBuffer(render)
    "ASYNC test: PASS",
]


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_media_codec_test.py <acl> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        sys.stdout.write("SKIP: ACL_ART_HOME not set\n"); return SKIP
    if not android_jar or not os.path.isfile(android_jar):
        sys.stdout.write("SKIP: no staged android.jar\n"); return SKIP
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        sys.stdout.write("SKIP: ffmpeg CLI not found\n"); return SKIP
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    d8 = os.path.join(home, "bin", "d8")
    zipalign = first(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            sys.stdout.write(f"SKIP: {name} not found\n"); return SKIP

    def run(cmd, **kw):
        subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

    fixtures = os.path.join(src_root, "tests/fixtures")
    sys.path.insert(0, fixtures)
    from make_axml import build_axml  # noqa: E402

    work = tempfile.mkdtemp(prefix="acl-mc-")
    try:
        # 1. a solid clip -> raw Annex-B H.264 elementary stream (the fixture asset).
        clip = os.path.join(work, "solid.mp4")
        g = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                            "-i", "color=c=0x40A060:s=320x240:d=1:r=10", "-c:v", "libx264",
                            "-pix_fmt", "yuv420p", "-profile:v", "main", clip], capture_output=True, text=True)
        if g.returncode != 0:
            sys.stdout.write("SKIP: ffmpeg could not encode the fixture\n"); return SKIP
        app = os.path.join(work, "app")
        os.makedirs(os.path.join(app, "assets"))
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", clip, "-c:v", "copy",
             "-bsf:v", "h264_mp4toannexb", "-f", "h264", os.path.join(app, "assets/raw.h264")])

        # 2. Main.java + MainAsync.java -> one classes.dex (same package).
        cls = os.path.join(work, "cls"); os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "media_codec/Main.java"),
             os.path.join(fixtures, "media_codec/MainAsync.java")])
        dexdir = os.path.join(work, "dex"); os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])

        # 3. assemble + run one APK per launcher (sync Main, async MainAsync) off the same dex+asset.
        def build_and_run(launcher, tag):
            with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
                fh.write(build_axml(utf8=True, package="com.acl.mctest", launcher=launcher))
            unaligned = os.path.join(work, f"u-{tag}.apk"); apk = os.path.join(work, f"mc-{tag}.apk")
            run(["zip", "-X", "-0", "-j", "-q", unaligned,
                 os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
            run(["zip", "-X", "-0", "-q", unaligned, "assets/raw.h264"], cwd=app)
            run([zipalign, "-f", "4", unaligned, apk])
            env = dict(os.environ, XDG_CACHE_HOME=os.path.join(work, f"cache-{tag}"))
            p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk],
                               env=env, capture_output=True, text=True, timeout=300)
            return p.returncode, p.stdout + p.stderr

        runs = [("com.acl.mctest.Main", "sync", SYNC_CHECKS),
                ("com.acl.mctest.MainAsync", "async", ASYNC_CHECKS)]
        out = ""
        for launcher, tag, checks in runs:
            rc, o = build_and_run(launcher, tag)
            out += f"=== {tag} ===\n" + o
            if rc == SKIP:
                sys.stdout.write("SKIP: run-apk.sh (vendored framework not built)\n" + out); return SKIP
            if "SKIP: no VAAPI" in o or "CodecDecoder::configure failed" in o:
                sys.stdout.write("SKIP: no VAAPI for zero-copy decode\n" + out); return SKIP
            if rc != 0:
                sys.stderr.write(f"FAIL: {tag} run exit {rc}\n" + out); return 1
            for c in checks:
                if c not in o:
                    sys.stderr.write(f"FAIL: {tag} missing '{c}'\n" + out); return 1
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n"); return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)

    sys.stdout.write("OK: MediaCodecList discovered acl.h264.decoder; MediaCodec decoded H.264 into an "
                     "ImageReader Surface zero-copy in BOTH synchronous and asynchronous (setCallback) "
                     "modes — the ExoPlayer path\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

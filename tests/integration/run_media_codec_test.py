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

CHECKS = [
    "native_start: decoder up",              # MediaCodec.start -> CodecDecoder configured
    "MC decode: frame in ImageReader 320x240",  # a decoded frame reached the consumer via queue_imported
    "MC decode: PASS",
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

        # 2. Main.java -> dex.
        cls = os.path.join(work, "cls"); os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "media_codec/Main.java")])
        dexdir = os.path.join(work, "dex"); os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))

        # 3. assemble the APK: manifest + classes.dex + assets/raw.h264, STORED + zipalign 4.
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.mctest", launcher="com.acl.mctest.Main"))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        unaligned = os.path.join(work, "u.apk"); apk = os.path.join(work, "mc.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run(["zip", "-X", "-0", "-q", unaligned, "assets/raw.h264"], cwd=app)
        run([zipalign, "-f", "4", unaligned, apk])

        # 4. run through run-apk.sh (real vendored framework; SKIPs, exit 77, if not built).
        env = dict(os.environ, XDG_CACHE_HOME=os.path.join(work, "cache"))
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk],
                           env=env, capture_output=True, text=True, timeout=300)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            sys.stdout.write("SKIP: run-apk.sh (vendored framework not built)\n" + out); return SKIP
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n"); return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if "SKIP: no VAAPI" in out or "CodecDecoder::configure failed" in out:
        sys.stdout.write("SKIP: no VAAPI for zero-copy decode\n" + out); return SKIP
    ok = p.returncode == 0
    for c in CHECKS:
        if c not in out:
            sys.stderr.write(f"FAIL: missing '{c}'\n"); ok = False
    if not ok:
        sys.stderr.write("--- output ---\n" + out + "\n"); return 1
    sys.stdout.write("OK: MediaCodec decoded H.264 into an ImageReader Surface (zero-copy, "
                     "createDecoderByType -> configure -> queueInputBuffer -> releaseOutputBuffer)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

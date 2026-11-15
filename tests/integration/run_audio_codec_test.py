# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; FFmpeg CLI to encode the fixture;
# tests/fixtures/audio_codec/Main.java. No GPL-3.0 sources.
#
# An audio decoder in android.media.MediaCodec, end to end: a fixture app finds a decoder for
# AAC through MediaCodecList, feeds it the access units of a 2 s 440 Hz tone, and writes the
# PCM it returns to an AudioTrack, as a media player does with a sound track. It then
# flushes the codec and decodes the first ten units again, as after a seek.
#
# Run four ways: the codec driven by dequeue calls and by a MediaCodec.Callback, each
# single-process and under --split. Asserted in all four:
#   - the decoder a4l advertises is found and can be made;
#   - the output format is announced before the first buffer: 48 kHz, stereo, 16-bit;
#   - every access unit comes out (95 x 1024 frames), the stream ends, and what came out is
#     the 440 Hz tone at its level;
#   - after the flush, ten units come out and the stream is not marked ended;
#   - the AudioTrack played all of it.
# Under --split, also that nothing was dropped or refused by the broker.
#
# The audio sink is the counting one (ACL_AUDIO unset): no sound. Needs a host ART build
# (ACL_ART_HOME), the AOSP toolchain, the ffmpeg CLI with an AAC encoder, FFmpeg libraries
# with an AAC decoder, bubblewrap and a Wayland compositor (it starts a headless Weston
# unless ACL_TEST_WAYLAND_DISPLAY names one). Exits 77 (skip) where any is missing.
#
# ACL_TEST_AUDIOCODEC_KEEP=<dir> keeps the APKs and the runs' logs there.
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77
UNITS = 95            # 2 s of 1024-frame AAC units at 48 kHz, as the CLI's encoder cuts them
FIRST_PASS = UNITS * 1024
AFTER_FLUSH = 10 * 1024  # Main.AFTER_FLUSH access units
HZ = 440


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
    name = f"a4l-audiocodec-test-{os.getpid()}"
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
        sys.stderr.write("usage: run_audio_codec_test.py <a4l> <android.jar>\n")
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
        return skip("ffmpeg CLI not found (encodes the fixture)")
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

    keep = os.environ.get("ACL_TEST_AUDIOCODEC_KEEP", "")
    work = tempfile.mkdtemp(prefix="a4l-audiocodec-")
    weston = None
    try:
        # 1. The tone, as ADTS.
        app = os.path.join(work, "app")
        os.makedirs(os.path.join(app, "assets"))
        g = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
             "-i", f"sine=frequency={HZ}:sample_rate=48000:duration=2", "-ac", "2",
             "-c:a", "aac", "-b:a", "128k", "-f", "adts", os.path.join(app, "assets/tone.aac")],
            capture_output=True, text=True)
        if g.returncode != 0:
            return skip("the ffmpeg CLI could not encode AAC", g.stderr)

        # 2. The fixture, as two APKs off one dex.
        cls = os.path.join(work, "cls")
        os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "audio_codec/Main.java"),
             os.path.join(fixtures, "audio_codec/MainAsync.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])

        def build_apk(launcher, tag):
            with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
                fh.write(build_axml(utf8=True, package="com.acl.audiocodec", launcher=launcher))
            unaligned = os.path.join(work, f"u-{tag}.apk")
            apk = os.path.join(work, f"audio-codec-{tag}.apk")
            run(["zip", "-X", "-0", "-j", "-q", unaligned,
                 os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
            run(["zip", "-X", "-0", "-q", unaligned, "assets/tone.aac"], cwd=app)
            run([zipalign, "-f", "4", unaligned, apk])
            return apk

        variants = [("dequeue", build_apk("com.acl.audiocodec.Main", "dequeue")),
                    ("callback", build_apk("com.acl.audiocodec.MainAsync", "callback"))]

        # 3. A compositor.
        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        env = {k: v for k, v in os.environ.items() if k not in ("ACL_AUDIO", "PIPEWIRE_NODE")}
        env.update(WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="90", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))

        def check(out):
            """What must hold in every run. Returns an error or None."""
            if "MC: FAILED" in out or "MC: done" not in out:
                return "the fixture did not finish"
            if "MC: decoder acl.aac.decoder" not in out:
                return "the AAC decoder was not found through MediaCodecList"
            f = re.search(r"MC: format (\d+) Hz, (\d+) ch, encoding (\d+), mime (\S+)", out)
            if not f:
                return "no output format was announced"
            if (f.group(1), f.group(2), f.group(3), f.group(4)) != ("48000", "2", "2", "audio/raw"):
                return f"the output format was {f.group(0)}"
            a = re.search(r"MC: first pass decoded (\d+) frames, hz (\d+), peak (\d+), eos (\w+)", out)
            b = re.search(r"MC: after flush decoded (\d+) frames, hz (\d+), peak (\d+), eos (\w+)", out)
            h = re.search(r"MC: played head (\d+) of (\d+)", out)
            if not (a and b and h):
                return "the fixture's report is incomplete"
            if int(a.group(1)) != FIRST_PASS or a.group(4) != "true":
                return f"first pass: {a.group(1)} frames of {FIRST_PASS}, eos {a.group(4)}"
            # By zero crossings over the whole pass, the encoder's start-up included.
            if abs(int(a.group(2)) - HZ) > 15 or not 2300 <= int(a.group(3)) <= 3500:
                return f"first pass: {a.group(2)} Hz at peak {a.group(3)}"
            if int(b.group(1)) != AFTER_FLUSH or b.group(4) != "false":
                return f"after the flush: {b.group(1)} frames of {AFTER_FLUSH}, eos {b.group(4)}"
            if not 2300 <= int(b.group(3)) <= 3500:
                return f"after the flush: peak {b.group(3)}"
            total = FIRST_PASS + AFTER_FLUSH
            if int(h.group(1)) != total or int(h.group(2)) != total:
                return f"the track played {h.group(1)} of {total}"
            return None

        notes = []
        for tag, apk in variants:
            runner = [os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off"]
            for mode, args in (("single process", ["--present"]),
                               ("--split", ["--split", "--present"])):
                p = subprocess.run(runner + args, env=env, capture_output=True, text=True,
                                   timeout=200)
                out = p.stdout + p.stderr
                if keep:
                    os.makedirs(keep, exist_ok=True)
                    shutil.copy(apk, keep)
                    with open(os.path.join(keep, f"run-{tag}-{mode.strip('-').replace(' ', '-')}.log"),
                              "w") as fh:
                        fh.write(out)
                if p.returncode == SKIP:
                    return skip("run-apk.sh (vendored framework not built)", out[-2000:])
                if "audio decoders for nothing" in out or "MC: decoder null" in out:
                    return skip("the FFmpeg libraries have no AAC decoder", out[-2000:])
                if mode == "--split":
                    if "confinement is not available" in out or "cannot confine" in out:
                        return skip("the app process cannot be confined here", out[-2000:])
                    if "needs the GPU present path" in out:
                        notes.append(f"{tag}: --split not built")
                        continue
                err = check(out)
                if err:
                    sys.stderr.write(f"FAIL ({tag}, {mode}): {err}\n{out[-6000:]}\n")
                    return 1
                if mode == "--split":
                    s = re.search(r"split: app process ended \((\w+)\); .*?(\d+) message\(s\) "
                                  r"rejected, .*?(\d+) audio frame\(s\) to the audio sink and "
                                  r"(\d+) dropped", out)
                    if not s or s.group(1) != "exited" or int(s.group(2)) != 0 \
                            or int(s.group(4)) != 0 or int(s.group(3)) != FIRST_PASS + AFTER_FLUSH:
                        sys.stderr.write(f"FAIL ({tag}, --split): the broker's account is "
                                         f"{s.group(0) if s else 'missing'}\n{out[-4000:]}\n")
                        return 1
                notes.append(f"{tag}, {mode}")
        sys.stdout.write(f"OK: AAC decoded through MediaCodec to an AudioTrack, {FIRST_PASS} + "
                         f"{AFTER_FLUSH} frames, in each of: " + "; ".join(notes) + "\n")
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

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness (docs/adr/ADR-0009, audio); FFmpeg CLI for the fixture
# tone; tests/fixtures/sound_pool/Main.java. No GPL-3.0 sources.
#
# Audio across the --split boundary. A fixture app plays a tone through SoundPool. Under
# `a4l run --split` the app process has no audio device: it mixes the sound and sends the
# samples to the broker, which checks them and gives them to its audio sink. The test asserts
# that every frame the app sent reached the broker's sink, that none was dropped or refused,
# and that the count is what the same app writes single-process.
#
# The sink is the counting one (ACL_AUDIO unset), so the test makes no sound and needs no
# audio daemon. Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, ffmpeg, bubblewrap
# and a Wayland compositor; it starts a headless Weston on its own socket unless
# ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip) where any of these is missing.
#
# ACL_TEST_AUDIO_KEEP=<dir> keeps the APK and the runs' logs there.
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77
TONE_SECONDS = 2  # a 1 s tone, played twice


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
    name = f"a4l-audio-test-{os.getpid()}"
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
        sys.stderr.write("usage: run_split_audio_test.py <a4l> <android.jar>\n")
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

    keep = os.environ.get("ACL_TEST_AUDIO_KEEP", "")
    work = tempfile.mkdtemp(prefix="a4l-audio-")
    weston = None
    try:
        # 1. A 440 Hz tone, 1 s, 48 kHz stereo, 16-bit.
        app = os.path.join(work, "app")
        os.makedirs(os.path.join(app, "assets"))
        g = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
             "-i", "sine=frequency=440:sample_rate=48000:duration=1", "-ac", "2",
             "-c:a", "pcm_s16le", os.path.join(app, "assets/tone.wav")],
            capture_output=True, text=True)
        if g.returncode != 0:
            return skip("ffmpeg could not make the fixture tone", g.stderr)

        # 2. The fixture APK. The asset is stored, not deflated: SoundPool loads it by
        #    descriptor and offset.
        cls = os.path.join(work, "cls")
        os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "sound_pool/Main.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.soundpool",
                                launcher="com.acl.soundpool.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "sound-pool.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run(["zip", "-X", "-0", "-q", unaligned, "assets/tone.wav"], cwd=app)
        run([zipalign, "-f", "4", unaligned, apk])

        # 3. A compositor.
        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        env = {k: v for k, v in os.environ.items() if k not in ("ACL_AUDIO", "PIPEWIRE_NODE")}
        env.update(WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        runner = [os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off"]

        def launch(args, tag):
            p = subprocess.run(runner + args, env=env, capture_output=True, text=True,
                               timeout=180)
            out = p.stdout + p.stderr
            if keep:
                os.makedirs(keep, exist_ok=True)
                shutil.copy(apk, keep)
                with open(os.path.join(keep, f"run-{tag}.log"), "w") as fh:
                    fh.write(out)
            return p.returncode, out

        def fail(why, out):
            sys.stderr.write(f"FAIL: {why}\n{out[-6000:]}\n")
            return 1

        # 4. Single process: what the app writes when the sink is its own.
        rc, out = launch(["--present"], "single")
        if rc == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        if "android.media.SoundPool natives registered" not in out:
            return skip("libsoundpool.so is not built (needs FFmpeg)", out[-2000:])
        if "AUDIO: playing, stream" not in out or "AUDIO: done" not in out:
            return fail("the fixture did not play single-process", out)
        m = re.search(r"sink=null close: (\d+) frames written", out)
        if not m:
            return fail("no sink count single-process", out)
        single = int(m.group(1))
        rate = re.search(r"sink=null open: (\d+) Hz, (\d+) ch", out)
        hz = int(rate.group(1)) if rate else 44100
        if single < TONE_SECONDS * hz:
            return fail(f"single-process wrote {single} frames, under {TONE_SECONDS} s", out)

        # 5. Across the boundary.
        rc, out = launch(["--split", "--present"], "split")
        if "confinement is not available" in out or "cannot confine" in out:
            return skip("the app process cannot be confined here", out[-2000:])
        if "needs the GPU present path" in out:
            return skip("built without the GPU present path")
        if "AUDIO: playing, stream" not in out or "AUDIO: done" not in out:
            return fail("the fixture did not play under --split", out)
        if "sink=split open" not in out:
            return fail("the app process did not send its audio to the broker", out)
        m = re.search(r"sink=split close: (\d+) frames sent", out)
        if not m:
            return fail("the app process did not report what it sent", out)
        sent = int(m.group(1))
        s = re.search(r"split: app process ended \((\w+)\); .*?(\d+) message\(s\) rejected, .*?"
                      r"(\d+) audio frame\(s\) to the audio sink and (\d+) dropped", out)
        if not s:
            return fail("no broker summary", out)
        how, rejected, got, dropped = s.group(1), int(s.group(2)), int(s.group(3)), int(s.group(4))
        if how != "exited":
            return fail(f"the app process {how}", out)
        if rejected != 0:
            return fail(f"the broker rejected {rejected} message(s)", out)
        if dropped != 0:
            return fail(f"the broker dropped {dropped} audio frame(s)", out)
        if got != sent:
            return fail(f"the app sent {sent} frames and the broker's sink got {got}", out)
        if sent < TONE_SECONDS * hz:
            return fail(f"the app sent {sent} frames, under {TONE_SECONDS} s", out)
        # The mixer runs for as long as the pool lives, so the two counts differ by how long
        # each run took to release it: within a quarter of a second.
        if abs(sent - single) > hz // 4:
            return fail(f"--split sent {sent} frames, single-process wrote {single}", out)
        sys.stdout.write(f"OK: {sent} frames at {hz} Hz sent by the app process, {got} at the "
                         f"broker's sink, 0 dropped, 0 rejected; single-process wrote {single}\n")
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

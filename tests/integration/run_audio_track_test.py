# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/audio_track/Main.java. No GPL-3.0
# sources.
#
# android.media.AudioTrack on host ART, single-process and across the --split boundary.
#
# A fixture app plays two streaming tracks with blocking writes: 2 s at 48 kHz stereo 16-bit
# (as short[], byte[] and a direct ByteBuffer) and 0.5 s at 44.1 kHz mono float. The test
# asserts, in both modes:
#   - every frame written reached the audio sink, and the playback position reached the end;
#   - a ToneGenerator's tone reached the sink whole, and an unknown tone type was refused;
#   - the blocking writes took about as long as the audio they wrote. A track whose writes
#     return at once plays nothing in time and lets a player run through its whole stream in
#     an instant, so this is the property a media player depends on.
# Under --split it also asserts that the broker's sink got exactly what the app process sent,
# with nothing dropped or refused.
#
# The sink is the counting one (ACL_AUDIO unset), so the test makes no sound and needs no
# audio daemon. Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, bubblewrap and a
# Wayland compositor; it starts a headless Weston on its own socket unless
# ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip) where any of these is missing.
#
# ACL_TEST_AUDIOTRACK_KEEP=<dir> keeps the APK and the runs' logs there.
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77
A_FRAMES = 96000  # track A: 2 s at 48 kHz (Main.trackA)
B_FRAMES = 22050  # track B: 0.5 s at 44.1 kHz (Main.trackB)
TONE_FRAMES = 19200  # TONE_PROP_ACK: 400 ms at the tone player's 48 kHz (Main.tone)


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
    name = f"a4l-audiotrack-test-{os.getpid()}"
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
        sys.stderr.write("usage: run_audio_track_test.py <a4l> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    if not android_jar or not os.path.isfile(android_jar):
        return skip("no staged android.jar")
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

    keep = os.environ.get("ACL_TEST_AUDIOTRACK_KEEP", "")
    work = tempfile.mkdtemp(prefix="a4l-audiotrack-")
    weston = None
    try:
        # 1. The fixture APK.
        app = os.path.join(work, "app")
        os.makedirs(app)
        cls = os.path.join(work, "cls")
        os.makedirs(cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "audio_track/Main.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.audiotrack",
                                launcher="com.acl.audiotrack.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "audio-track.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        # 2. A compositor.
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

        def check(tag, out, sink):
            """What must hold in either mode. Returns (error, summary)."""
            if "android.media.AudioTrack natives registered" not in out:
                return "AudioTrack's natives were not registered", None
            if "UnsatisfiedLinkError" in out:
                return "an AudioTrack native is missing", None
            if "AT: FAILED" in out or "AT: done" not in out:
                return "the fixture did not finish", None
            a = re.search(r"AT: A wrote (\d+) frames in (\d+) ms", out)
            ah = re.search(r"AT: A head (\d+) of (\d+), timestamp (-?\d+)", out)
            b = re.search(r"AT: B wrote (\d+) frames in (\d+) ms", out)
            bh = re.search(r"AT: B head (\d+) of (\d+)", out)
            if not (a and ah and b and bh):
                return "the fixture's report is incomplete", None
            if int(a.group(1)) != A_FRAMES or int(b.group(1)) != B_FRAMES:
                return f"wrote {a.group(1)} and {b.group(1)} frames", None
            a_ms, b_ms = int(a.group(2)), int(b.group(2))
            # 2 s and 0.5 s of audio, less the track's 80 ms buffer and the engine's lead. A
            # write that does not block returns in a few milliseconds; a slow machine only
            # makes it longer.
            if not 1500 <= a_ms <= 4000:
                return f"track A's 2 s of blocking writes took {a_ms} ms", None
            if not 250 <= b_ms <= 2000:
                return f"track B's 0.5 s blocking write took {b_ms} ms", None
            if int(ah.group(1)) != A_FRAMES or int(bh.group(1)) != B_FRAMES:
                return (f"the playback position stopped at {ah.group(1)} of {A_FRAMES} and "
                        f"{bh.group(1)} of {B_FRAMES}"), None
            if not 0 < int(ah.group(3)) <= A_FRAMES:
                return f"track A's timestamp was {ah.group(3)}", None
            counts = [int(n) for n in re.findall(rf"sink={sink} close: (\d+) frames", out)]
            if "AT: tone started true, unknown false, session true, after release false" not in out:
                return "ToneGenerator did not start, refuse and release as it should", None
            if counts != [A_FRAMES, B_FRAMES, TONE_FRAMES]:
                return f"the {sink} sink closed with {counts}", None
            return None, f"{tag}: writes took {a_ms} ms and {b_ms} ms"

        def fail(why, out):
            sys.stderr.write(f"FAIL: {why}\n{out[-6000:]}\n")
            return 1

        # 3. Single process.
        rc, out = launch(["--present"], "single")
        if rc == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        err, single = check("single process", out, "null")
        if err:
            return fail(f"single process: {err}", out)

        # 4. Across the boundary.
        rc, out = launch(["--split", "--present"], "split")
        if "confinement is not available" in out or "cannot confine" in out:
            return skip("the app process cannot be confined here", out[-2000:])
        if "needs the GPU present path" in out:
            sys.stdout.write(f"OK: {single}; --split not built (no GPU present path)\n")
            return 0
        err, split = check("--split", out, "split")
        if err:
            return fail(f"--split: {err}", out)
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
        sent = A_FRAMES + B_FRAMES + TONE_FRAMES
        if got != sent:
            return fail(f"the broker's sink got {got} of {sent} frames", out)
        sys.stdout.write(f"OK: {A_FRAMES} + {B_FRAMES} frames through AudioTrack; {single}; "
                         f"{split}, {got} frames at the broker's sink, 0 dropped, 0 rejected\n")
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

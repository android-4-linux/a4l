# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/font_fallback/Main.java; the Weston
# helpers of run_gl_surfaceview_test.py. No GPL-3.0 sources.
#
# A character the Paint's typeface has no glyph for is drawn and measured from another
# font, where a4l drew the box of a missing glyph. The fixture measures and draws text with
# the katakana middle dot (U+30FB) in it, which Roboto does not have, and logs what it
# finds. It also measures a String with a character past ASCII, which a4l measured by
# applying the String's indexes to its UTF-8.
#
# Run one names the fallback font (ACL_FONT_FALLBACK): Noto Sans CJK from the AOSP tree,
# in which the dot is one em wide. Run two names none, and the font is the host's
# fontconfig's to find. It is made where fc-list says the host has a font with the dot.
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain and its noto-fonts, Weston
# with its kiosk shell, and a render node. Exits 77 (skip) where any of these is missing.
#
# Usage: run_font_fallback_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from run_gl_surfaceview_test import first, start_weston
from run_video_plane_test import skip

COMMON = [
    "FONT: past ASCII: string and chars agree=true widths add up=true A after it=true",
    "FONT: hasGlyph dot=true A=true U+10FFFE=false",
    "FONT: dot drawn: ink=true a dot=true",
    "FONT: run drawn: B ends at the run's advance=true",
    "FONT: done",
]
# Noto Sans CJK's dot is one em: the text size.
NAMED = COMMON + ["FONT: dot advance=100 run adds up=true widths=true"]
HOST = COMMON + [" run adds up=true widths=true"]


def story_of(out):
    return "\n".join(ln for ln in out.splitlines() if "FONT: " in ln)


def host_has_the_dot():
    """Whether the host's fontconfig lists a font with U+30FB."""
    lister = shutil.which("fc-list")
    if not lister:
        return False
    r = subprocess.run([lister, ":charset=30fb", "file"], capture_output=True, text=True)
    return r.returncode == 0 and bool(r.stdout.strip())


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_font_fallback_test.py <a4l> <android.jar>\n")
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
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "")
    if not runtime_dir or not os.path.isdir(runtime_dir):
        return skip("XDG_RUNTIME_DIR not set")
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    cjk = os.path.join(root, "external/noto-fonts/cjk/NotoSansCJK-Regular.ttc")
    if not os.path.isfile(cjk):
        return skip("no Noto Sans CJK in the AOSP tree")
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
    work = tempfile.mkdtemp(prefix="a4l-font-")
    weston = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, appdir, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "font_fallback/Main.java")])
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package="com.acl.fontfallback",
                                launcher="com.acl.fontfallback.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "font.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)

        runs = [("named", {"ACL_FONT_FALLBACK": cjk}, NAMED)]
        host = "the host's fontconfig has no font with the dot: that run was not made"
        if host_has_the_dot():
            runs.append(("host", {}, HOST))
            host = "found by the host's fontconfig too"
        for tag, extra, checks in runs:
            env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60",
                       ACL_BIN=acl, ACL_PRESENT_GPU="1",
                       XDG_CACHE_HOME=os.path.join(work, tag, "cache"),
                       XDG_DATA_HOME=os.path.join(work, tag, "data"))
            env.pop("ACL_FONT_FALLBACK", None)
            env.update(extra)
            p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                "--verify=off", "--present"], env=env,
                               capture_output=True, text=True, errors="replace", timeout=120)
            out = p.stdout + p.stderr
            if p.returncode == 77:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            for c in checks:
                if c not in out or "FONT: FAILED" in out:
                    sys.stderr.write(f"FAIL ({tag}): missing '{c}'\n{story_of(out)}\n"
                                     f"{out[-3000:]}\n")
                    return 1
            sys.stdout.write(f"{tag}:\n{story_of(out)}\n")
        sys.stdout.write("OK: a character the typeface does not have was measured and "
                         f"drawn from a font named for it; {host}\n")
        return 0
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    except subprocess.TimeoutExpired as e:
        sys.stderr.write(f"FAIL: the app did not end: {e}\n")
        return 1
    finally:
        if weston is not None and weston.poll() is None:
            try:
                os.killpg(weston.pid, 15)
                weston.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                weston.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

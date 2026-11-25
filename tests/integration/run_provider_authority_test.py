# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/provider_authority. No GPL-3.0 sources.
#
# A content provider whose android:authorities is a string resource is reached by the
# authority the string holds. The fixture's manifest gives "@string/authority"; its Activity
# asks the ContentResolver for the type of a URI under that authority and exits 0 when the
# provider answered.
#
# The fixture is built with aapt2, since it has a resource table; the other fixtures are a
# manifest and a dex.
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain with aapt2 and framework-res.apk,
# a render node and a Wayland compositor. It starts a headless Weston on its own socket unless
# ACL_TEST_WAYLAND_DISPLAY names one. Exits 77 (skip) where any of these is missing.
#
# Usage: run_provider_authority_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from run_notify_test import first, skip, start_weston, stop

SKIP = 77
WANT = "AUTHORITY: com.acl.authres.files type=text/x-fixture"


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_provider_authority_test.py <a4l> <android.jar>\n")
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
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    d8 = os.path.join(home, "bin", "d8")
    aapt2 = os.path.join(home, "bin", "aapt2")
    zipalign = first(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("d8", d8), ("aapt2", aapt2), ("zipalign", zipalign),
                       ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            return skip(f"{name} not found")
    fres = sorted(glob.glob(os.path.join(
        root, "out/target/product/*/system/framework/framework-res.apk")))
    if not fres:
        return skip("framework-res.apk not built")

    def run(cmd, **kw):
        subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

    fixture = os.path.join(src_root, "tests/fixtures/provider_authority")
    work = tempfile.mkdtemp(prefix="a4l-authority-")
    weston = None
    try:
        gen = os.path.join(work, "gen")
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        for d in (gen, cls, dexdir):
            os.makedirs(d)
        compiled = os.path.join(work, "res.zip")
        linked = os.path.join(work, "linked.apk")
        run([aapt2, "compile", "--dir", os.path.join(fixture, "res"), "-o", compiled])
        run([aapt2, "link", "-o", linked, "--manifest",
             os.path.join(fixture, "AndroidManifest.xml"), "-I", fres[0], "--java", gen,
             compiled])
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixture, "Main.java"),
             os.path.join(gen, "com/acl/authres/R.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["zip", "-X", "-0", "-j", "-q", linked, os.path.join(dexdir, "classes.dex")])
        apk = os.path.join(work, "authority.apk")
        run([zipalign, "-f", "4", linked, apk])

        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)
        env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                   ACL_PRESENT_GPU="1", ACL_AUDIO="null",
                   XDG_CACHE_HOME=os.path.join(work, "cache"),
                   XDG_DATA_HOME=os.path.join(work, "data"))
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off",
                            "--present"], env=env, capture_output=True, text=True,
                           timeout=200)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            return skip("run-apk.sh (vendored framework not built)", out[-2000:])
        story = "\n".join(ln for ln in out.splitlines()
                          if "AUTHORITY: " in ln or "ContentProvider" in ln)
        if WANT not in out or p.returncode != 0:
            sys.stderr.write(f"FAIL: exit {p.returncode}; wanted '{WANT}'\n{story}\n")
            return 1
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        stop(weston)
        shutil.rmtree(work, ignore_errors=True)

    sys.stdout.write("OK: a provider with a string-resource authority answered under the "
                     "string's value\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

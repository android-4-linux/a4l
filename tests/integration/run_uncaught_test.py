# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/uncaught/Main.java. No GPL-3.0 sources.
#
# An exception no thread catches is logged as Android's runtime logs it, "FATAL EXCEPTION:
# <thread>" with the exception, and the app goes on. The fixture runs three ways:
#   - a thread throws: the log names the thread and the exception, and the main thread is
#     still running half a second later
#   - a thread hands the exception to its handler itself, as kotlinx.coroutines does: the same
#   - a thread calls System.exit(3) with ACL_EXIT_STACKS=1: the exit status is 3 and the log
#     names the thread that called it
#
# Needs a host ART build (ACL_ART_HOME), the AOSP toolchain, a render node and a Wayland
# compositor. It starts a headless Weston on its own socket unless ACL_TEST_WAYLAND_DISPLAY
# names one. Exits 77 (skip) where any of these is missing.
#
# Usage: run_uncaught_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from run_notify_test import first, skip, start_weston, stop

SKIP = 77
PACKAGE = "com.acl.uncaught"
RUNS = [
    ("throw", 0, ["FATAL EXCEPTION: worker-x", "java.lang.IllegalStateException: boom",
                  "UNCAUGHT: still running"]),
    ("direct", 0, ["FATAL EXCEPTION: worker-d", "java.lang.IllegalStateException: boom direct",
                   "UNCAUGHT: still running"]),
    ("exit", 3, ["acl exit: called on thread quitter",
                 "acl exit:   at java.lang.System.exit"]),
]


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_uncaught_test.py <a4l> <android.jar>\n")
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
    zipalign = first(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            return skip(f"{name} not found")

    def run(cmd, **kw):
        subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

    fixtures = os.path.join(src_root, "tests/fixtures")
    work = tempfile.mkdtemp(prefix="a4l-uncaught-")
    weston = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        app = os.path.join(work, "app")
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        for d in (app, cls, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "uncaught/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main"))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "uncaught.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        display = os.environ.get("ACL_TEST_WAYLAND_DISPLAY", "")
        if not display:
            weston, display = start_weston(runtime_dir, os.path.join(work, "weston.log"))
            if weston is None:
                return skip(display)

        for how, status, lines in RUNS:
            env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60", ACL_BIN=acl,
                       ACL_PRESENT_GPU="1", ACL_AUDIO="null", ACL_TEST_END=how,
                       XDG_CACHE_HOME=os.path.join(work, "cache"),
                       XDG_DATA_HOME=os.path.join(work, "data"))
            if how == "exit":
                env["ACL_EXIT_STACKS"] = "1"
            else:
                env.pop("ACL_EXIT_STACKS", None)
            p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                "--verify=off", "--present"], env=env, capture_output=True,
                               text=True, timeout=200)
            out = p.stdout + p.stderr
            if p.returncode == SKIP:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            story = "\n".join(ln for ln in out.splitlines()
                              if "UNCAUGHT" in ln or "FATAL" in ln or "acl exit" in ln
                              or "IllegalState" in ln)
            for want in lines:
                if want not in out:
                    sys.stderr.write(f"FAIL ({how}): the log lacks '{want}'\n{story}\n")
                    return 1
            if p.returncode != status:
                sys.stderr.write(f"FAIL ({how}): exit status {p.returncode}, not {status}\n"
                                 f"{story}\n")
                return 1
            if how != "exit" and "acl exit:" in out:
                sys.stderr.write(f"FAIL ({how}): exit stacks printed without "
                                 f"ACL_EXIT_STACKS\n{story}\n")
                return 1
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: build step: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        stop(weston)
        shutil.rmtree(work, ignore_errors=True)

    sys.stdout.write("OK: an uncaught exception is logged and the app goes on, and "
                     "ACL_EXIT_STACKS names a thread that calls System.exit\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

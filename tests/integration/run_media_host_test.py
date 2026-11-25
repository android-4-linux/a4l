# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/media_host/Main.java. No GPL-3.0 sources.
#
# `a4l run --service=<class>` hosts a media browser service that is not a MediaBrowserService
# by class. The fixture's service extends Service and hands out the binder of a
# MediaBrowserService it keeps inside, as androidx's MediaBrowserServiceCompat does; under R8
# that androidx class is renamed and a lookup by name finds neither base. a4l binds it with
# the framework's MediaBrowser. The checks: the service is given this package and this
# process's uid as its client, the browser reads the root, and the root's two children come
# back with their flags and titles.
#
# The fixture also checks its MediaSession: a MediaController reads the metadata and playback
# state it set and is told when they change, the queue it set comes back item for item and
# is gone when it is set to none, the session manager lists it as active, and the
# controller's play, pause, skipToNext, seekTo and playFromMediaId reach its callback.
#
# Needs a host ART build (ACL_ART_HOME) and the AOSP toolchain. Exits 77 (skip) without them.
#
# Usage: run_media_host_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile

SKIP = 77
PACKAGE = "com.acl.mediahost"
SERVICE = f"{PACKAGE}.Main$Wrapped"
# In order.
WANT = [
    "MEDIAHOST: onGetRoot client known=true",
    "acl media: browse root: root",
    "acl media: children of 'root': 2 item(s)",
    "acl media:   [browse] albums 'Albums'",
    "acl media:   [play] track-1 'Track 1'",
]


# The session line comes when the main looper gets to it, so its place is not checked.
SESSION = ("MEDIAHOST: session read=true told=15 queued=true emptied=true active=1 "
           "commands=play pause next seek42000 id:track-1")


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_media_host_test.py <a4l> <android.jar>\n")
        return 2
    acl, android_jar = sys.argv[1], sys.argv[2]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
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
    work = tempfile.mkdtemp(prefix="a4l-media-host-")
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        app = os.path.join(work, "app")
        cls = os.path.join(work, "cls")
        dexdir = os.path.join(work, "dex")
        for d in (app, cls, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "media_host/Main.java")])
        run([d8, "--min-api", "26", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main",
                                services=[(SERVICE, ["android.media.browse.MediaBrowserService"])]))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, "mediahost.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        env = dict(os.environ, ACL_BIN=acl, ACL_RUN_TIMEOUT="120", ACL_AUDIO="null")
        p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk, "--verify=off",
                            f"--service={SERVICE}"],
                           env=env, capture_output=True, text=True, timeout=200)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            return skip("tools/run-apk.sh skipped", out[-2000:])
        story = "\n".join(ln for ln in out.splitlines()
                          if "MEDIAHOST: " in ln or "acl media" in ln or "host service" in ln)
        at = 0
        for want in WANT:
            at = out.find(want, at)
            if at < 0:
                sys.stderr.write(f"FAIL: missing, or out of order: {want}\n{story}\n")
                return 1
        if SESSION not in out:
            sys.stderr.write(f"FAIL: missing: {SESSION}\n{story}\n")
            return 1
        if p.returncode != 0:
            sys.stderr.write(f"FAIL: a4l exited {p.returncode}\n{story}\n")
            return 1
        print("PASS: a media browser service that is not one by class was bound and browsed")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/fixtures/signing/Main.java; the APK signer of
# tests/fixtures/make_signed_apks.py; the Weston helpers of run_gl_surfaceview_test.py. No
# GPL-3.0 sources.
#
# The certificate an app is signed with, as the app sees it. a4l gave an app none:
# PackageInfo.signatures and signingInfo were null, and hasSigningCertificate threw "not
# implemented in subclass", which ended Spotify 9 in its first Activity.
#
# The fixture APK is signed here with APK Signature Scheme v2 and a key made for the run, and
# run three ways:
#   signed    the app's signatures hold one certificate, its SHA-256 is the key's, signingInfo
#             holds the same, and hasSigningCertificate says yes to it (as a digest, whole,
#             and by uid) and no to another digest and to another package
#   unsigned  the APK as it was built: no certificates, hasSigningCertificate says no, and
#             nothing throws
#   named     the unsigned APK is run and ACL_APK_SIGNED names the signed one, as
#             tools/run-apk.sh does for a bundle whose base APK it copies: the answers are
#             the signed run's
#
# Needs: a host ART build (ACL_ART_HOME), the AOSP toolchain, openssl, Weston with its kiosk
# shell, and a render node. Exits 77 (skip) where any of these is missing.
#
# Usage: run_signing_test.py <a4l> <android.jar>
import glob
import os
import shutil
import subprocess
import sys
import tempfile

from run_gl_surfaceview_test import first, start_weston
from run_video_plane_test import skip

PACKAGE = "com.acl.signing"
NONE = "SIGNING: signatures=0 signingInfo=false has=false unasked=true"


def story_of(out):
    return "\n".join(ln for ln in out.splitlines()
                     if "SIGNING: " in ln or "signing certificates" in ln)


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_signing_test.py <a4l> <android.jar>\n")
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
    if not shutil.which("openssl"):
        return skip("openssl not found")
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
    work = tempfile.mkdtemp(prefix="a4l-signing-")
    weston = None
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        import make_signed_apks as signer  # noqa: E402
        cls, appdir, dexdir = (os.path.join(work, d) for d in ("cls", "app", "dex"))
        for d in (cls, appdir, dexdir):
            os.makedirs(d)
        run([javac, "--release", "8", "-cp", android_jar, "-d", cls,
             os.path.join(fixtures, "signing/Main.java")])
        run([d8, "--min-api", "33", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))
        run(["cp", os.path.join(dexdir, "classes.dex"), appdir])
        with open(os.path.join(appdir, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main"))
        unaligned = os.path.join(work, "u.apk")
        unsigned = os.path.join(work, "unsigned.apk")
        signed = os.path.join(work, "signed.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(appdir, "AndroidManifest.xml"), os.path.join(appdir, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, unsigned])
        # Scheme v2, ECDSA with SHA-256, a key of this run's.
        key = signer.Key(work, "ec", ["-algorithm", "EC", "-pkeyopt",
                                      "ec_paramgen_curve:P-256"])
        with open(unsigned, "rb") as fh:
            z = fh.read()
        with open(signed, "wb") as fh:
            fh.write(signer.with_block(
                z, [(signer.V2, signer.block_value(signer.signer(z, key, [0x0201])))]))
        yes = (f"SIGNING: signatures=1 sha256={key.cert_sha256} signingInfo=true flags33=true"
               " has=true/true/true wrong=false other=false unasked=true")

        weston, display = start_weston(runtime_dir, work)
        if weston is None:
            return skip(display)
        for tag, apk, extra, want in (("signed", signed, {}, yes),
                                      ("unsigned", unsigned, {}, NONE),
                                      ("named", unsigned, {"ACL_APK_SIGNED": signed}, yes)):
            env = dict(os.environ, WAYLAND_DISPLAY=display, ACL_RUN_TIMEOUT="60",
                       ACL_BIN=acl, ACL_PRESENT_GPU="1",
                       XDG_CACHE_HOME=os.path.join(work, tag, "cache"),
                       XDG_DATA_HOME=os.path.join(work, tag, "data"))
            env.pop("ACL_APK_SIGNED", None)
            env.update(extra)
            p = subprocess.run([os.path.join(src_root, "tools/run-apk.sh"), apk,
                                "--verify=off", "--present"], env=env,
                               capture_output=True, text=True, errors="replace", timeout=120)
            out = p.stdout + p.stderr
            if p.returncode == 77:
                return skip("run-apk.sh (vendored framework not built)", out[-2000:])
            if want not in out or "SIGNING: done" not in out:
                sys.stderr.write(f"FAIL ({tag}): missing '{want}'\n{story_of(out)}\n"
                                 f"{out[-3000:]}\n")
                return 1
            sys.stdout.write(f"{tag}:\n{story_of(out)}\n")
        sys.stdout.write("OK: an app sees the certificate its APK is signed with, none where "
                         "it is unsigned, and the named APK's where one is named\n")
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

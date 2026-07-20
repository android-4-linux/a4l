# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B); the NDK ANativeWindow contract
# (android/native_window.h, public). Mirrors run_ndk_art_test.py's build recipe but runs the app
# through tools/run-apk.sh (the REAL vendored framework boot classpath is needed for
# android.media.ImageReader, unlike run_ndk_art_test.py's android.jar stub). No GPL-3.0 sources.
#
# The ANativeWindow-producer end-to-end ([[flutter-texture-bridge]] M3c): a bionic-ABI
# lib/x86_64/libanwtest.so, loaded in-process by acl's bionic loader, drives one frame through the
# real NDK ANativeWindow C ABI (fromSurface -> lock -> fill -> unlockAndPost) into an
# android.media.ImageReader whose consumer/BufferQueue is acl's, and the Java side acquireLatestImage
# es it. So the asserted "ANW produce: lockW=64 image=true" proves the producer mapped a real 64x64
# pooled dma-buf across the bionic->host %fs boundary AND the post reached the consumer.
#
# Needs a host ART build ($ACL_ART_HOME), the AOSP host toolchain (clang/d8/zipalign/javac), a built
# bionic ($AOSP, for the linker/sysroot/stub + the target .so), and the vendored framework (via
# run-apk.sh). SKIPs (exit 77) when any is absent, so CI stays green without them.
#
# Usage: run_anw_producer_test.py <acl> <source_root> <android.jar>
import glob
import os
import subprocess
import sys
import tempfile

SKIP = 77  # meson interprets exit 77 as "skipped"

CHECKS = [
    "produced 64x64 stride=64",              # the bionic native ran fromSurface+lock+fill+unlockAndPost
    "nativeImageSetup ok",                    # the consumer acquired the posted frame
    "ANW produce: lockW=64 image=true",       # Java: lock mapped a real 64x64 buffer + acquire got it
]


def first_executable(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_anw_producer_test.py <acl> <src_root> <android.jar>\n")
        return 2
    acl, src_root, android_jar = sys.argv[1:]

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        sys.stdout.write("SKIP: ACL_ART_HOME not set (host ART build required)\n")
        return SKIP
    if not android_jar or not os.path.isfile(android_jar):
        sys.stdout.write("SKIP: no staged android.jar (framework stub layer)\n")
        return SKIP

    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    clang = first_executable(
        os.path.join(root, "prebuilts/clang/host/linux-x86/clang-r450784d/bin/clang"))
    d8 = os.path.join(home, "bin", "d8")
    zipalign = first_executable(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first_executable(
        os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
        os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("clang", clang), ("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            sys.stdout.write(f"SKIP: {name} not found under {root}\n")
            return SKIP

    # bionic intermediates the .so links against (full paths -> bionic sonames, no glibc leak).
    inter = os.path.join(root, "out/soong/.intermediates")
    libc = os.path.join(inter, "bionic/libc/libc/android_x86_64_shared/libc.so")
    liblog = os.path.join(inter, "system/logging/liblog/liblog/android_x86_64_shared/liblog.so")
    libdl = os.path.join(inter, "bionic/libdl/libdl/android_x86_64_shared/libdl.so")
    crtbegin = os.path.join(inter, "bionic/libc/crtbegin_so/android_x86_64/crtbegin_so.o")
    crtend = os.path.join(
        inter, "bionic/libc/crtend_so/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend_so.o")
    inc_jni = os.path.join(root, "libnativehelper/include_jni")
    inc_log = os.path.join(root, "system/logging/liblog/include")
    for f in (libc, liblog, libdl, crtbegin, crtend):
        if not os.path.exists(f):
            sys.stdout.write(f"SKIP: no built bionic ({os.path.basename(f)} absent under {inter})\n")
            return SKIP

    def run(cmd, env=None, cwd=None):
        subprocess.run(cmd, check=True, capture_output=True, text=True, env=env, cwd=cwd)

    fixtures = os.path.join(src_root, "tests/fixtures")
    sys.path.insert(0, fixtures)
    from make_axml import build_axml  # noqa: E402

    work = tempfile.mkdtemp(prefix="acl-anw-")
    try:
        tools = os.path.join(src_root, "tools")
        tool_env = dict(os.environ, AOSP=root)
        # 1. stage the bionic runtime (linker + sysroot + acl's libandroid.so the .so links).
        stage = os.path.join(work, "bionic-runtime")
        try:
            run([os.path.join(tools, "stage-bionic-runtime.sh"), stage], env=tool_env)
        except subprocess.CalledProcessError as e:
            sys.stdout.write(f"SKIP: stage-bionic-runtime failed ({e.stderr.strip().splitlines()[-1:]})\n")
            return SKIP
        libandroid = os.path.join(stage, "bionic", "libandroid.so")
        if not os.path.isfile(libandroid):
            sys.stdout.write("SKIP: staged libandroid.so absent\n")
            return SKIP

        # 2. the bionic-ABI producer .so (links libandroid.so so ANativeWindow_* resolve to acl's).
        so = os.path.join(work, "libanwtest.so")
        run([clang, "--target=x86_64-linux-android31", "-fPIC", "-shared", "-nostdlib",
             "-fno-emulated-tls", "-Wl,-soname,libanwtest.so", "-I", inc_jni, "-I", inc_log,
             "-o", so, crtbegin, os.path.join(fixtures, "art_ndk_anw/anwtest.c"), crtend,
             libc, liblog, libdl, libandroid, "-fuse-ld=lld"])

        # 3. Main.java -> dex.
        app_cls = os.path.join(work, "cls")
        os.makedirs(app_cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", app_cls,
             os.path.join(fixtures, "art_ndk_anw/Main.java")])
        dexdir = os.path.join(work, "dex")
        os.makedirs(dexdir)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", dexdir]
            + glob.glob(os.path.join(app_cls, "**/*.class"), recursive=True))

        # 4. assemble the APK: manifest + classes.dex + lib/x86_64/libanwtest.so, STORED + zipalign 4.
        app = os.path.join(work, "app")
        os.makedirs(os.path.join(app, "lib/x86_64"))
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as f:
            f.write(build_axml(utf8=True, package="com.acl.anwtest", launcher="com.acl.anwtest.Main"))
        run(["cp", os.path.join(dexdir, "classes.dex"), app])
        run(["cp", so, os.path.join(app, "lib/x86_64/")])
        unaligned = os.path.join(work, "unaligned.apk")
        apk = os.path.join(work, "anw.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run(["zip", "-X", "-0", "-q", unaligned, "lib/x86_64/libanwtest.so"], cwd=app)
        run([zipalign, "-f", "4", unaligned, apk])

        # 5. run it on host ART with the REAL framework via run-apk.sh (SKIPs, exit 77, if the
        #    vendored framework isn't built). Keep lib extraction + cache inside the sandbox.
        env = dict(os.environ, XDG_CACHE_HOME=os.path.join(work, "cache"))
        p = subprocess.run([os.path.join(tools, "run-apk.sh"), apk],
                           env=env, capture_output=True, text=True, timeout=300)
        out = p.stdout + p.stderr
        if p.returncode == SKIP:
            sys.stdout.write("SKIP: run-apk.sh (vendored framework not built)\n")
            sys.stdout.write(out)
            return SKIP
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: fixture build step failed: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        subprocess.run(["rm", "-rf", work], check=False)

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: acl run exit {p.returncode}\n")
    for c in CHECKS:
        if c not in out:
            sys.stderr.write(f"FAIL: missing '{c}'\n")
            ok = False
    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: bionic lib/x86_64/libanwtest.so drove one ANativeWindow producer frame "
                     "(fromSurface+lock+unlockAndPost) into an acl ImageReader; the consumer "
                     "acquired it — producer path proven end-to-end\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

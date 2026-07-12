# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/art-host-spike.md
# (M3 step 1); AOSP d8/zipalign host tooling. No GPL-3.0 sources.
#
# Gate G2.d on REAL host ART: prove acl runs a real Activity's lifecycle through the
# --vm=art backend (host libart, DEX loaded natively), not just the JVM fallback.
# This needs a host ART build ($ACL_ART_HOME) + the AOSP host toolchain (d8, zipalign,
# jdk). When they are absent (normal CI) the test SKIPs (exit 77) rather than failing.
#
# What it does when ACL_ART_HOME is set:
#   1. d8 the vendored framework -> DEX. acl's build compiles the shims with the host
#      javac (Java 25 / class v69), which android-13's d8 rejects, so recompile the
#      shim SOURCES at --release 8, then d8 (shim dex) + d8 android.jar (stub dex).
#   2. Compile the MainActivity fixture -> d8 -> zipalign into an APK (ART mmaps the
#      dex and requires 4-byte alignment; it reuses activity.apk's binary manifest).
#   3. Run `acl run <apk> --vm=art` with ACL_ART_FRAMEWORK=<shim.dex>:<stub.dex> and
#      assert the Activity lifecycle ran on ART.
#
# Usage: run_activity_art_test.py <acl> <source_root> <android.jar> <activity.apk>
import glob
import os
import subprocess
import sys
import tempfile

SKIP = 77  # meson interprets exit 77 as "skipped"

CHECKS = [
    "launching Activity com.acl.hello.MainActivity",   # Activity-lifecycle path taken
    "getPackageName()=com.acl.hello",                  # real manifest package injected
    "MainActivity.onCreate ran on host ART",           # the app's onCreate ran on ART
]


def first_executable(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write(
            "usage: run_activity_art_test.py <acl> <src_root> <android.jar> <activity.apk>\n")
        return 2
    acl, src_root, android_jar, activity_apk = sys.argv[1:5]

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        sys.stdout.write("SKIP: ACL_ART_HOME not set (host ART build required)\n")
        return SKIP
    if not android_jar or not os.path.isfile(android_jar):
        sys.stdout.write("SKIP: no staged android.jar (framework stub layer)\n")
        return SKIP

    # AOSP tree root is three levels above out/host/linux-x86.
    root = os.path.abspath(os.path.join(home, "..", "..", ".."))
    d8 = os.path.join(home, "bin", "d8")
    # linux*-x86 only — prebuilts/build-tools also ships darwin-x86 (sorts first).
    zipalign = first_executable(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first_executable(
        os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
        os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            sys.stdout.write(f"SKIP: {name} not found under {root}\n")
            return SKIP

    def run(cmd):
        subprocess.run(cmd, check=True, capture_output=True, text=True)

    work = tempfile.mkdtemp(prefix="acl-art-activity-")
    try:
        # 1. framework shim dex (recompile sources at --release 8, then d8)
        shim_srcs = (glob.glob(os.path.join(src_root, "framework/shim/**/*.java"), recursive=True)
                     + glob.glob(os.path.join(src_root, "third_party/aosp/frameworks/**/*.java"),
                                 recursive=True))
        shim_cls = os.path.join(work, "shimclasses")
        os.makedirs(shim_cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", shim_cls] + shim_srcs)
        shim_dex = os.path.join(work, "shim")
        os.makedirs(shim_dex)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", shim_dex]
            + glob.glob(os.path.join(shim_cls, "**/*.class"), recursive=True))
        # 2. android.jar stub dex
        stub_dex = os.path.join(work, "stub")
        os.makedirs(stub_dex)
        run([d8, "--min-api", "24", "--output", stub_dex, android_jar])
        # 3. app: MainActivity fixture -> dex -> zipaligned apk (reuse activity.apk manifest)
        app_java = os.path.join(src_root, "tests/fixtures/art_activity/MainActivity.java")
        app_cls = os.path.join(work, "appclasses")
        os.makedirs(app_cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", app_cls, app_java])
        app_dex = os.path.join(work, "app")
        os.makedirs(app_dex)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", app_dex]
            + glob.glob(os.path.join(app_cls, "**/*.class"), recursive=True))
        run(["unzip", "-o", activity_apk, "AndroidManifest.xml", "-d", app_dex])
        unaligned = os.path.join(work, "unaligned.apk")
        apk = os.path.join(work, "activity_art.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app_dex, "AndroidManifest.xml"),
             os.path.join(app_dex, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        # 4. run on host ART
        env = dict(os.environ)
        env["ACL_ART_FRAMEWORK"] = (os.path.join(shim_dex, "classes.dex") + ":"
                                    + os.path.join(stub_dex, "classes.dex"))
        p = subprocess.run([acl, "run", apk, "--vm=art"],
                           env=env, capture_output=True, text=True, timeout=180)
        out = p.stdout + p.stderr
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: fixture build step failed: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        # best-effort cleanup; leave on failure for debugging is unnecessary here
        subprocess.run(["rm", "-rf", work], check=False)

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: acl run --vm=art exit {p.returncode}\n")
    for c in CHECKS:
        if c not in out:
            sys.stderr.write(f"FAIL: missing '{c}'\n")
            ok = False
    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: real Activity lifecycle ran on host ART (--vm=art)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

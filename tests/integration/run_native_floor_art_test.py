# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/phase2-scope.md (P2.2),
# ADR-0008; AOSP framework-minus-apex + host libandroid_runtime (Apache-2.0). No GPL-3.0 sources.
#
# P2.2 / ADR-0008 on REAL host ART: prove acl registers the real framework native floor
# from the host libandroid_runtime (--vm=art with ACL_ART_NATIVE) and a framework native
# executes. Needs a host ART build ($ACL_ART_HOME) with libandroid_runtime.so AND the
# real framework DEX (framework-minus-apex javalib.jar, built by `m framework-minus-apex`).
# SKIPs (exit 77) when any are absent (normal CI), so it never fails the build.
#
# Steps: locate the real framework jar in the ART tree; d8 the Main fixture into a
# zipaligned APK; run `acl run <apk> --vm=art` with ACL_ART_BOOTCLASSPATH=<real framework>
# and ACL_ART_NATIVE=1; assert the native floor registered and SystemProperties executed.
#
# Usage: run_native_floor_art_test.py <acl> <source_root>
import glob
import os
import subprocess
import sys
import tempfile

SKIP = 77

CHECKS = [
    "ART native floor:",                                # acl registered libandroid_runtime natives
    "framework native group(s) from libandroid_runtime",
    "FRAMEWORK-FLOOR PROBE: android.os.SystemProperties classloader=BOOT",  # P2.1: on bootclasspath
    "FRAMEWORK-FLOOR PROBE: SystemProperties.get(ro.build.version.sdk)=33", # P2.2: native + seeded prop
    "FRAMEWORK-FLOOR PROBE: Build.CPU_ABI=x86_64",                          # A(1): build.prop seeded
    "FRAMEWORK-FLOOR PROBE: android:string/ok=OK",                         # R1+R3: AssetManager + framework-res
]

# The framework boot jars (PRODUCT_BOOT_JARS, system), in order.
BOOT_JAR_MODULES = ["framework-minus-apex", "framework-graphics", "ext",
                    "telephony-common", "voip-common", "ims-common"]


def first_file(*pats):
    for pat in pats:
        for p in sorted(glob.glob(pat)):
            if os.path.isfile(p):
                return p
    return None


def first_executable(*pats):
    for pat in pats:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_native_floor_art_test.py <acl> <src_root>\n")
        return 2
    acl, src_root = sys.argv[1], sys.argv[2]

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        sys.stdout.write("SKIP: ACL_ART_HOME not set (host ART build required)\n")
        return SKIP
    if not os.path.isfile(os.path.join(home, "lib64", "libandroid_runtime.so")):
        sys.stdout.write("SKIP: no host libandroid_runtime.so\n")
        return SKIP

    root = os.path.abspath(os.path.join(home, "..", "..", ".."))
    # Assemble the framework BOOTCLASSPATH from the built boot jars (P2.1). All must be
    # present; framework-minus-apex is the anchor others resolve against.
    boot_jars = []
    for mod in BOOT_JAR_MODULES:
        jar = first_file(
            os.path.join(root, "out/target/product/*/obj/JAVA_LIBRARIES/"
                               f"{mod}_intermediates/javalib.jar"),
            os.path.join(root, f"out/soong/.intermediates/frameworks/base/{mod}/*/*/javalib.jar"))
        if jar:
            boot_jars.append(jar)
    framework = ":".join(boot_jars) if boot_jars else None
    # framework-res.apk (the android:R.* resource data) — needed for R1+R3 resolution.
    framework_res = first_file(
        os.path.join(root, "out/target/product/*/system/framework/framework-res.apk"))
    if not framework_res:
        sys.stdout.write("SKIP: framework-res.apk not built (m framework-res)\n")
        return SKIP
    if not framework:
        sys.stdout.write("SKIP: framework boot jars not built (m framework-minus-apex ...)\n")
        return SKIP
    d8 = os.path.join(home, "bin", "d8")
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

    work = tempfile.mkdtemp(prefix="acl-art-nativefloor-")
    try:
        # Main fixture -> dex -> zipaligned apk (reuse any binary manifest with a
        # com.acl.hello.Main launcher; build a fresh one via aapt-free path is overkill,
        # so run with --main to skip manifest launcher resolution).
        app_java = os.path.join(src_root, "tests/fixtures/art_native/Main.java")
        app_cls = os.path.join(work, "cls")
        os.makedirs(app_cls)
        run([javac, "--release", "8", "-d", app_cls, app_java])
        app_dex = os.path.join(work, "dex")
        os.makedirs(app_dex)
        run([d8, "--min-api", "24", "--output", app_dex]
            + glob.glob(os.path.join(app_cls, "**/*.class"), recursive=True))
        # Minimal apk: just classes.dex (acl --main names the entry; no manifest needed
        # for a static-main run — pkg reads the zip, --main overrides launcher lookup).
        unaligned = os.path.join(work, "unaligned.apk")
        apk = os.path.join(work, "native_floor.apk")
        run(["zip", "-X", "-0", "-j", "-q", unaligned, os.path.join(app_dex, "classes.dex")])
        run([zipalign, "-f", "4", unaligned, apk])

        env = dict(os.environ)
        env["ACL_ART_BOOTCLASSPATH"] = framework  # P2.1: framework on the bootclasspath
        env["ACL_ART_NATIVE"] = "1"
        env["ACL_ART_FRAMEWORK_RES"] = framework_res  # R3: framework-res.apk resource data
        p = subprocess.run([acl, "run", apk, "--vm=art", "--main=com.acl.hello.Main"],
                           env=env, capture_output=True, text=True, timeout=180)
        out = p.stdout + p.stderr
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: fixture build step failed: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
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
    sys.stdout.write("OK: real framework native floor registered + executed on host ART\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

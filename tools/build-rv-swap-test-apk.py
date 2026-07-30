#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl diagnostic tool; AOSP host toolchain (javac/d8/zipalign, Apache-2.0) +
# androidx prebuilts from AOSP prebuilts/maven_repo (Apache-2.0). No GPL-3.0 sources.
#
# Build the RecyclerView content-swap repro (tests/fixtures/rv_swap_test/MainActivity.java): a real
# androidx RecyclerView whose adapter is refreshed (list<->skeleton, repopulate/rebind) on a timer —
# the faithful podcast Discover white-out repro. Pulls the recyclerview AAR + its transitive androidx
# deps from AOSP prebuilts/maven_repo, extracts each classes.jar, compiles + d8's everything.
# Run it:  tools/run-apk.sh <out.apk> --present   (with the usual ACL_* env; ACL_SWAP_MS to tune)
#
# Usage: build-rv-swap-test-apk.py [out.apk]   (default: /tmp/acl/rvswap.apk)
import glob
import os
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "fixtures"))
from make_axml import build_axml  # noqa: E402

AOSP = os.environ.get("ACL_AOSP_ROOT", "/mnt/dev/aosp")
ANDROID_JAR = os.path.join(AOSP, "prebuilts/sdk/33/public/android.jar")
MAVEN = os.path.join(AOSP, "prebuilts/maven_repo/android/androidx")

# (module_dir, version) — recyclerview + its transitive androidx deps. Extra deps are harmless (d8
# just includes the classes); missing ones surface as javac/runtime ClassNotFound, add them here.
DEPS = [
    ("recyclerview/recyclerview", "1.0.0-beta01"),
    ("core/core", "1.0.0-beta01"),
    ("customview/customview", "1.0.0-beta01"),
    ("collection/collection", "1.0.0-beta01"),
    ("versionedparcelable/versionedparcelable", "1.0.0-beta01"),
    ("annotation/annotation", "1.0.0"),
    ("interpolator/interpolator", "1.0.0-beta01"),
    ("lifecycle/lifecycle-common", "2.0.0-beta01"),
    ("lifecycle/lifecycle-runtime", "2.0.0-beta01"),
    ("lifecycle/lifecycle-viewmodel", "2.0.0-beta01"),
    ("arch/core/core-common", "2.0.0-beta01"),
    ("arch/core/core-runtime", "2.0.0-beta01"),
]


def first(*pats):
    for pat in pats:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def dep_jar(work, module, version):
    """Return the path to a usable classes jar for a dep (extracts classes.jar from an .aar)."""
    base = os.path.join(MAVEN, module, version)
    name = os.path.basename(module) + "-" + version
    aar = os.path.join(base, name + ".aar")
    jar = os.path.join(base, name + ".jar")
    if os.path.isfile(jar):
        return jar
    if os.path.isfile(aar):
        out = os.path.join(work, "dep-" + os.path.basename(module) + ".jar")
        with zipfile.ZipFile(aar) as z:
            with z.open("classes.jar") as src, open(out, "wb") as dst:
                dst.write(src.read())
        return out
    sys.stderr.write(f"WARN: dep not found: {module} {version}\n")
    return None


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/acl/rvswap.apk"
    d8 = first(os.path.join(AOSP, "out/host/linux-x86/bin/d8"))
    zipalign = first(os.path.join(AOSP, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(AOSP, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(AOSP, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for n, t in (("android.jar", ANDROID_JAR if os.path.isfile(ANDROID_JAR) else None),
                 ("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not t:
            sys.stderr.write(f"missing tool: {n}\n"); return 2

    work = "/tmp/acl/rvswap-build"
    subprocess.run(["rm", "-rf", work], check=False)
    os.makedirs(work)

    def run(cmd):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write("CMD FAILED: " + " ".join(cmd[:3]) + " ...\n" + r.stderr[-3000:] + "\n")
            raise SystemExit(1)
        return r

    dep_jars = [j for j in (dep_jar(work, m, v) for m, v in DEPS) if j]
    print(f"gathered {len(dep_jars)} androidx dep jars")

    axml = build_axml(package="com.acl.rvswap", launcher=".MainActivity", permissions=[])
    with open(os.path.join(work, "AndroidManifest.xml"), "wb") as f:
        f.write(axml)

    src = os.path.join(ROOT, "tests/fixtures/rv_swap_test/MainActivity.java")
    cls = os.path.join(work, "cls"); os.makedirs(cls)
    cp = os.pathsep.join([ANDROID_JAR] + dep_jars)
    run([javac, "--release", "8", "-cp", cp, "-d", cls, src])

    dex = os.path.join(work, "dex"); os.makedirs(dex)
    # d8 my classes + all androidx dep jars (multidex auto if needed).
    run([d8, "--min-api", "24", "--lib", ANDROID_JAR, "--output", dex]
        + glob.glob(os.path.join(cls, "**/*.class"), recursive=True) + dep_jars)

    dexfiles = sorted(glob.glob(os.path.join(dex, "*.dex")))
    print(f"d8 produced {len(dexfiles)} dex file(s): {[os.path.basename(d) for d in dexfiles]}")

    unaligned = os.path.join(work, "unaligned.apk")
    try:
        os.remove(out)
    except FileNotFoundError:
        pass
    run(["zip", "-X", "-0", "-j", "-q", unaligned,
         os.path.join(work, "AndroidManifest.xml")] + dexfiles)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    run([zipalign, "-f", "4", unaligned, out])
    print(f"built {out} ({os.path.getsize(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

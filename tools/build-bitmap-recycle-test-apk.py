#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl diagnostic tool; AOSP host toolchain (javac/d8/zipalign,
# Apache-2.0). No GPL-3.0 sources.
#
# Build the clipToOutline (rounded-corner) repro (tests/fixtures/bitmap_recycle_test/MainActivity.java):
# a white FrameLayout holding a centerd opaque-BLUE View with a round-rect ViewOutlineProvider +
# setClipToOutline(true). Exercises the RenderNode outline clip in acl's native tree walk — the
# corners must show the parent (white), not blue.
# Run it:  tools/run-apk.sh <out.apk> --present   (with the usual ACL_* env)
#
# Usage: build-outline-test-apk.py [out.apk]   (default: /tmp/acl/bmrecycle.apk)
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "fixtures"))
from make_axml import build_axml  # noqa: E402

AOSP = os.environ.get("ACL_AOSP_ROOT", "/mnt/dev/aosp")
ANDROID_JAR = os.path.join(AOSP, "prebuilts/sdk/33/public/android.jar")


def first(*pats):
    for pat in pats:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/acl/bmrecycle.apk"
    d8 = first(os.path.join(AOSP, "out/host/linux-x86/bin/d8"))
    zipalign = first(os.path.join(AOSP, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first(os.path.join(AOSP, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
                  os.path.join(AOSP, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for n, t in (("android.jar", ANDROID_JAR if os.path.isfile(ANDROID_JAR) else None),
                 ("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not t:
            sys.stderr.write(f"missing tool: {n}\n"); return 2

    work = "/tmp/acl/bmrecycle-build"
    subprocess.run(["rm", "-rf", work], check=False)
    os.makedirs(work)

    def run(cmd):
        subprocess.run(cmd, check=True, capture_output=True, text=True)

    # 1. binary manifest (launcher MainActivity, no permissions needed)
    axml = build_axml(package="com.acl.bmrecycle", launcher=".MainActivity", permissions=[])
    with open(os.path.join(work, "AndroidManifest.xml"), "wb") as f:
        f.write(axml)

    # 2. javac -> d8 -> classes.dex
    src = os.path.join(ROOT, "tests/fixtures/bitmap_recycle_test/MainActivity.java")
    cls = os.path.join(work, "cls"); os.makedirs(cls)
    run([javac, "--release", "8", "-cp", ANDROID_JAR, "-d", cls, src])
    dex = os.path.join(work, "dex"); os.makedirs(dex)
    run([d8, "--min-api", "24", "--lib", ANDROID_JAR, "--output", dex]
        + glob.glob(os.path.join(cls, "**/*.class"), recursive=True))

    # 3. package + zipalign
    unaligned = os.path.join(work, "unaligned.apk")
    try:
        os.remove(out)
    except FileNotFoundError:
        pass
    run(["zip", "-X", "-0", "-j", "-q", unaligned,
         os.path.join(work, "AndroidManifest.xml"),
         os.path.join(dex, "classes.dex")])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    run([zipalign, "-f", "4", unaligned, out])
    print(f"built {out} ({os.path.getsize(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

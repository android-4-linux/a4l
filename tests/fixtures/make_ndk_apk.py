# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; AOSP androidfw binary-XML layout
# (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit the binary AndroidManifest.xml for the bionic-NDK end-to-end APK: package
# com.acl.ndktest, launcher activity com.acl.ndktest.Main (tests/fixtures/art_ndk/Main.java).
#
# Only the manifest is generated here. classes.dex (d8 of Main.java) and the bionic
# lib/x86_64/libndktest.so (tools/build-ndk-repro.sh) need the AOSP host toolchain, which
# is not a build dependency — run_ndk_art_test.py builds them at test time, SKIPping when
# the toolchain is absent, and assembles the final zipaligned APK around this manifest.
# Same split as run_activity_art_test.py, which reuses activity.apk's manifest.
#
# Usage: make_ndk_apk.py <output.apk>
import sys
import zipfile

from make_axml import build_axml

PACKAGE = "com.acl.ndktest"
LAUNCHER = "com.acl.ndktest.Main"


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_ndk_apk.py <output.apk>\n")
        return 2
    with zipfile.ZipFile(sys.argv[1], "w") as z:
        info = zipfile.ZipInfo("AndroidManifest.xml", date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED
        z.writestr(info, build_axml(utf8=True, package=PACKAGE, launcher=LAUNCHER))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

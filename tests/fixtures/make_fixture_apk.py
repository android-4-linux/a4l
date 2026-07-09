# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator. No GPL-3.0 sources.
#
# Emit a small, deterministic APK-shaped zip for the pkg/ reader tests.
# Usage: make_fixture_apk.py <output.apk>
#
# Contents exercise: DEFLATE + STORED entries, multi-dex discovery, and several
# lib/<abi>/ directories for native_abis().
import sys
import zipfile

from make_axml import build_axml

# (name, data, compression) — order is the central-directory order.
ENTRIES = [
    ("AndroidManifest.xml", build_axml(utf8=True), zipfile.ZIP_STORED),
    ("classes.dex", b"dex\n035\x00fake-dex-1", zipfile.ZIP_STORED),
    ("classes2.dex", b"dex\n035\x00fake-dex-2-" + b"x" * 200, zipfile.ZIP_DEFLATED),
    ("assets/hello.txt", b"hello aclcxx\n", zipfile.ZIP_DEFLATED),
    ("resources.arsc", b"\x02\x00\x0c\x00fake-arsc", zipfile.ZIP_STORED),
    ("lib/arm64-v8a/libfoo.so", b"\x7fELF-arm64", zipfile.ZIP_STORED),
    ("lib/armeabi-v7a/libfoo.so", b"\x7fELF-arm32", zipfile.ZIP_STORED),
    ("lib/x86_64/libfoo.so", b"\x7fELF-x86_64", zipfile.ZIP_STORED),
    ("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\n", zipfile.ZIP_STORED),
]


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_fixture_apk.py <output.apk>\n")
        return 2
    out = sys.argv[1]
    with zipfile.ZipFile(out, "w") as z:
        for name, data, method in ENTRIES:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = method
            z.writestr(info, data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

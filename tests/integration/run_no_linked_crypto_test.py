#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test. No GPL-3.0 sources.
#
# a4l must not link libcrypto or libssl. It shares a process with host ART,
# whose conscrypt uses AOSP's BoringSSL; a NEEDED entry for OpenSSL puts it in
# the global symbol scope first, BoringSSL's libssl binds to it, and every app
# faults in NativeCrypto.<clinit>. pkg/crypto_dl.cpp loads libcrypto privately
# instead. CI has no host ART to show the fault, so assert the cause is absent.
#
# Usage: run_no_linked_crypto_test.py <readelf> <a4l>
import re
import subprocess
import sys


def main() -> int:
    readelf, exe = sys.argv[1], sys.argv[2]
    out = subprocess.run([readelf, "-d", "-W", exe], check=True,
                         capture_output=True, text=True).stdout
    needed = re.findall(r"\(NEEDED\).*\[([^\]]+)\]", out)
    if not needed:
        sys.stdout.write("FAIL: no NEEDED entries read from %s\n" % exe)
        return 1
    bad = [n for n in needed if re.match(r"lib(crypto|ssl)\b", n)]
    if bad:
        sys.stdout.write("FAIL: a4l links %s\n" % ", ".join(bad))
        return 1
    sys.stdout.write("OK: %d NEEDED entries, none libcrypto or libssl\n" % len(needed))
    return 0


if __name__ == "__main__":
    sys.exit(main())

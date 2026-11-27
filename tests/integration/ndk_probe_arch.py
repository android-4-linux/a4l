# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test helper. No GPL-3.0 sources.
"""Which bionic an acl-ndk-probe binary needs, and how to run it on this machine.

The probe's ELF machine picks the architecture. An arm64 probe on an x86_64 machine runs under
qemu-aarch64-static with the sysroot named by ACL_ARM64_SYSROOT.
"""

import os
import platform
import shutil
import struct

EM = {62: "x86_64", 183: "arm64"}


def probe_arch(probe, aosp):
    """Returns (arch, wrapper, None), or (None, None, why) when the test cannot run here."""
    with open(probe, "rb") as fh:
        machine = struct.unpack_from("<H", fh.read(20), 18)[0]
    arch = EM.get(machine)
    if arch is None:
        return None, None, f"no bionic loader for ELF machine {machine}"
    linker = os.path.join(aosp, {
        "x86_64": "out/target/product/generic_x86_64/apex/com.android.runtime/bin/linker64",
        "arm64": "out/soong/.intermediates/bionic/linker/linker/linux_bionic_arm64/linker64",
    }[arch])
    if not os.path.exists(linker):
        return None, None, f"no built {arch} bionic linker under {aosp}"
    host = {"x86_64": "x86_64", "aarch64": "arm64"}.get(platform.machine())
    wrapper = []
    if host != arch:
        qemu = shutil.which("qemu-aarch64-static") if arch == "arm64" else None
        sysroot = os.environ.get("ACL_ARM64_SYSROOT", "/mnt/dev/sysroot-bullseye-arm64")
        if not qemu or not os.path.isdir(sysroot):
            return None, None, f"a {arch} probe on a {host} machine needs qemu-user and a sysroot"
        wrapper = [qemu, "-L", sysroot]
    return arch, wrapper, None

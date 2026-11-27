# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; docs/ndk-scope.md. No GPL-3.0 sources.
#
# The in-process bionic linker bootstraps, loads a library and calls into it, on the
# architecture a4l was built for: x86_64 or arm64. The test stages the runtime for the probe's
# architecture, builds tests/fixtures/ndk_answer.c against it, and runs
# `acl-ndk-probe <lib> answer`, which must print 42: the library's answer uses malloc, stdio,
# errno and a thread-local, so a wrong thread pointer crashes or gives another number. Then
# `acl-ndk-probe <lib> --thread`: a second thread is attached and gets 42 on a TCB of its own.
# Then `--boundary` and `--jni` with tests/fixtures/ndk_jni.c: a call up into the host and back,
# a JNI_OnLoad through a4l's wrapped JNIEnv, and registered natives through the down-call stub.
#
# An arm64 probe on an x86_64 machine runs under qemu-aarch64-static with the sysroot named by
# ACL_ARM64_SYSROOT (default /mnt/dev/sysroot-bullseye-arm64).
#
# Needs a built AOSP (bionic for the probe's architecture). SKIPs (exit 77) without one.
#
# Usage: run_ndk_bootstrap_test.py <acl-ndk-probe> <source_root>
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

SKIP = 77
EM = {62: "x86_64", 183: "arm64"}


def skip(why):
    sys.stdout.write(f"SKIP: {why}\n")
    return SKIP


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_ndk_bootstrap_test.py <acl-ndk-probe> <source_root>\n")
        return 2
    probe, src_root = sys.argv[1], sys.argv[2]
    with open(probe, "rb") as fh:
        machine = struct.unpack_from("<H", fh.read(20), 18)[0]
    arch = EM.get(machine)
    if arch is None:
        return skip(f"no bionic loader for ELF machine {machine}")
    aosp = os.environ.get("AOSP", "/mnt/dev/aosp")
    clang = os.path.join(aosp, "prebuilts/clang/host/linux-x86/clang-r450784d/bin/clang")
    inter = os.path.join(aosp, "out/soong/.intermediates")
    variant = "android_x86_64" if arch == "x86_64" else "linux_bionic_arm64"
    crtb = os.path.join(inter, f"bionic/libc/crtbegin_so/{variant}/crtbegin_so.o")
    crte = os.path.join(
        inter, f"bionic/libc/crtend_so/{variant}/obj/bionic/libc/arch-common/bionic/crtend_so.o")
    for need in (clang, crtb, crte):
        if not os.path.exists(need):
            return skip(f"no {os.path.relpath(need, aosp)} under {aosp}")

    host = {"x86_64": "x86_64", "aarch64": "arm64"}.get(platform.machine())
    wrapper = []
    if host != arch:
        qemu = shutil.which("qemu-aarch64-static") if arch == "arm64" else None
        sysroot = os.environ.get("ACL_ARM64_SYSROOT", "/mnt/dev/sysroot-bullseye-arm64")
        if not qemu or not os.path.isdir(sysroot):
            return skip(f"a {arch} probe on a {host} machine needs qemu-user and a sysroot")
        wrapper = [qemu, "-L", sysroot]

    work = tempfile.mkdtemp(prefix="a4l-ndk-boot-")
    try:
        stage = os.path.join(work, "stage")
        st = subprocess.run(
            [os.path.join(src_root, "tools/stage-bionic-runtime.sh"), stage],
            env=dict(os.environ, ACL_BIONIC_ARCH=arch), capture_output=True, text=True)
        if st.returncode != 0:
            return skip("staging failed: " + (st.stderr.strip().splitlines() or ["?"])[-1])
        triple = "x86_64-linux-android31" if arch == "x86_64" else "aarch64-linux-android31"
        asm = "asm-x86" if arch == "x86_64" else "asm-arm64"
        lib = os.path.join(work, "libanswer.so")
        cc = subprocess.run(
            [clang, f"--target={triple}", "-fPIC", "-shared", "-nostdlib", "-fno-emulated-tls",
             "-O1", "-nostdlibinc",
             "-I" + os.path.join(aosp, "bionic/libc/include"),
             "-I" + os.path.join(aosp, "bionic/libc/kernel/uapi"),
             "-I" + os.path.join(aosp, "bionic/libc/kernel/uapi", asm),
             "-I" + os.path.join(aosp, "bionic/libc/kernel/android/uapi"),
             "-Wl,-soname,libanswer.so", "-o", lib, crtb,
             "-I" + os.path.join(aosp, "libnativehelper/include_jni"),
             os.path.join(src_root, "tests/fixtures/ndk_answer.c"),
             os.path.join(src_root, "tests/fixtures/ndk_jni.c"), crte,
             os.path.join(stage, "bionic/libc.so"), "-fuse-ld=lld"],
            capture_output=True, text=True)
        if cc.returncode != 0:
            sys.stderr.write(f"FAIL: could not build the fixture library\n{cc.stderr}\n")
            return 1
        env = dict(os.environ,
                   ACL_BIONIC_LINKER=os.path.join(stage, "linker64"),
                   ACL_BIONIC_SYSROOT=os.path.join(stage, "bionic"),
                   ACL_BIONIC_STUB=os.path.join(stage, "stub"))
        for attempt in range(3):  # a thread pointer left wrong shows up as an odd run out
            p = subprocess.run(wrapper + [probe, lib, "answer"], env=env,
                               capture_output=True, text=True, timeout=120)
            out = p.stdout + p.stderr
            if p.returncode != 0 or "answer() = 42 (PASS)" not in out:
                sys.stderr.write(f"FAIL: run {attempt + 1}: exit {p.returncode}\n{out}\n")
                return 1
            # A second thread, attached: its own TCB, thread struct and tid, and the
            # library's thread-local at its initial value there.
            p = subprocess.run(wrapper + [probe, lib, "--thread"], env=env,
                               capture_output=True, text=True, timeout=120)
            out = p.stdout + p.stderr
            if p.returncode != 0 or "(PASS)" not in out:
                sys.stderr.write(f"FAIL: --thread run {attempt + 1}: exit {p.returncode}\n{out}\n")
                return 1
            # The boundary: a call up into the host and back, then a JNI_OnLoad and a
            # registered native through a4l's wrapped JNIEnv and its down-call stub, one of
            # them with arguments on the stack.
            for mode, want in (("--boundary", "boundary selftest = 1010 (PASS)"),
                               ("--jni", "native=4250 (varargs up-call) stack-args=ok -> (PASS)")):
                p = subprocess.run(wrapper + [probe, lib, mode], env=env,
                                   capture_output=True, text=True, timeout=120)
                out = p.stdout + p.stderr
                if p.returncode != 0 or want not in out:
                    sys.stderr.write(
                        f"FAIL: {mode} run {attempt + 1}: exit {p.returncode}\n{out}\n")
                    return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.stdout.write(f"OK: the bionic linker bootstrapped on {arch}; a library answered 42 on the "
                     "bootstrapping thread and on an attached one, and its JNI_OnLoad and "
                     "natives ran across the boundary\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/ndk-scope.md (attach-as-bionic).
# No GPL-3.0 sources.
#
# Per-thread attach-as-bionic: a thread glibc created, given a bionic TCB acl built by hand, must
# be able to run real bionic code AS ITSELF.
#
# `acl-ndk-probe --thread` attaches a second thread and, on it, asserts:
#   - nativeAnswer() == 42 — it calls __android_log_print, so liblog and bionic libc run on the
#     synthesized TCB and touch its TLS. A wrong TCB, or a wrong stack canary (it lives in a TCB
#     slot), crashes instead of returning.
#   - bionic's gettid() == the kernel's for that thread.
#   - pthread_self() is that thread's OWN pthread_internal_t, not the bootstrapping thread's —
#     that field backs recursive/errorcheck mutex ownership, so sharing it would break mutual
#     exclusion rather than fail loudly.
#
# Needs a staged bionic runtime + the repro .so, and acl built with the offsets header
# (ACL_BIONIC_OFFSETS_AVAILABLE, i.e. a built AOSP). SKIPs (exit 77) when absent.
#
# Usage: run_ndk_attach_test.py <acl-ndk-probe> <source_root>
import os
import subprocess
import sys
import tempfile

SKIP = 77


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_ndk_attach_test.py <acl-ndk-probe> <source_root>\n")
        return 2
    probe, src_root = sys.argv[1], sys.argv[2]
    aosp = os.environ.get("AOSP", "/mnt/dev/aosp")
    linker = os.path.join(
        aosp, "out/target/product/generic_x86_64/apex/com.android.runtime/bin/linker64")
    if not os.path.exists(linker):
        sys.stdout.write(f"SKIP: no built bionic linker under {aosp}\n")
        return SKIP

    work = tempfile.mkdtemp(prefix="acl-ndk-attach-")
    try:
        tools = os.path.join(src_root, "tools")
        env = dict(os.environ, AOSP=aosp)
        stage = os.path.join(work, "bionic-runtime")
        so_dir = os.path.join(work, "repro")
        try:
            subprocess.run([os.path.join(tools, "stage-bionic-runtime.sh"), stage],
                           check=True, capture_output=True, text=True, env=env)
            subprocess.run([os.path.join(tools, "build-ndk-repro.sh"), so_dir],
                           check=True, capture_output=True, text=True, env=env)
        except subprocess.CalledProcessError as e:
            sys.stdout.write(f"SKIP: cannot stage bionic ({os.path.basename(e.cmd[0])})\n")
            return SKIP

        env.update({
            "ACL_BIONIC_LINKER": os.path.join(stage, "linker64"),
            "ACL_BIONIC_SYSROOT": os.path.join(stage, "bionic"),
            "ACL_BIONIC_STUB": os.path.join(stage, "stub"),
        })
        p = subprocess.run([probe, os.path.join(so_dir, "libndktest.so"), "--thread"],
                           env=env, capture_output=True, text=True, timeout=120)
        out = p.stdout + p.stderr
    finally:
        subprocess.run(["rm", "-rf", work], check=False)

    # Built without the offsets header: attach is compiled out and says so. Not a failure of the
    # loader, just a build without an AOSP to measure — report it rather than pretending to pass.
    if "built without bionic offsets" in out:
        sys.stdout.write("SKIP: acl was built without the bionic offsets header\n")
        return SKIP
    if p.returncode != 0 or "(PASS)" not in out:
        sys.stderr.write(f"FAIL: --thread exit {p.returncode}\n--- output ---\n{out}\n")
        return 1
    sys.stdout.write("OK: a glibc-created thread attached to bionic and ran real bionic code "
                     "as itself (own TCB, own pthread_internal_t, right tid)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

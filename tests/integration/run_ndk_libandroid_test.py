# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/ndk-scope.md (attach-as-bionic).
# No GPL-3.0 sources.
#
# acl's own libandroid.so (src/ndk/bionic/acl_libandroid.c), staged into the bionic sysroot: an
# app's native code links libandroid.so, so without it a real engine's library cannot even load.
#
# This drives the ALooper family the way a real engine does — Flutter's engine threads each run an
# ALooper over real fds — from inside a bionic .so that has DT_NEEDED on libandroid.so:
#   - ALooper_prepare is idempotent per thread (forThread returns the same looper)
#   - a poll with nothing readable reports TIMEOUT rather than inventing an event
#   - ALooper_wake breaks a poll and reports WAKE, distinctly from a registered fd
#   - a write to a registered pipe reports back OUR ident, fd, events and data pointer
# i.e. a real epoll round trip, not merely a symbol that links.
#
# Needs a staged bionic runtime + the repro .so ($AOSP). SKIPs (exit 77) when absent.
#
# Usage: run_ndk_libandroid_test.py <acl-ndk-probe> <source_root>
import os
import subprocess
import sys
import tempfile

SKIP = 77


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_ndk_libandroid_test.py <acl-ndk-probe> <source_root>\n")
        return 2
    probe, src_root = sys.argv[1], sys.argv[2]
    aosp = os.environ.get("AOSP", "/mnt/dev/aosp")
    linker = os.path.join(
        aosp, "out/target/product/generic_x86_64/apex/com.android.runtime/bin/linker64")
    if not os.path.exists(linker):
        sys.stdout.write(f"SKIP: no built bionic linker under {aosp}\n")
        return SKIP

    work = tempfile.mkdtemp(prefix="acl-ndk-libandroid-")
    try:
        tools = os.path.join(src_root, "tools")
        env = dict(os.environ, AOSP=aosp)
        stage = os.path.join(work, "bionic-runtime")
        so_dir = os.path.join(work, "repro")
        try:
            subprocess.run([os.path.join(tools, "stage-bionic-runtime.sh"), stage],
                           check=True, capture_output=True, text=True, env=env)
            # Build the fixture against the runtime just staged: it links acl's libandroid.so.
            subprocess.run([os.path.join(tools, "build-ndk-repro.sh"), so_dir], check=True,
                           capture_output=True, text=True,
                           env=dict(env, ACL_BIONIC_SYSROOT=os.path.join(stage, "bionic")))
        except subprocess.CalledProcessError as e:
            sys.stdout.write(f"SKIP: cannot stage bionic ({os.path.basename(e.cmd[0])})\n")
            return SKIP

        env.update({
            "ACL_BIONIC_LINKER": os.path.join(stage, "linker64"),
            "ACL_BIONIC_SYSROOT": os.path.join(stage, "bionic"),
            "ACL_BIONIC_STUB": os.path.join(stage, "stub"),
        })
        p = subprocess.run([probe, os.path.join(so_dir, "libndktest.so"), "--looper"],
                           env=env, capture_output=True, text=True, timeout=120)
        out = p.stdout + p.stderr
    finally:
        subprocess.run(["rm", "-rf", work], check=False)

    if p.returncode != 0 or "(PASS)" not in out:
        sys.stderr.write(f"FAIL: --looper exit {p.returncode}\n--- output ---\n{out}\n")
        return 1
    sys.stdout.write("OK: acl's libandroid.so ALooper drives a real epoll round trip "
                     "(prepare/addFd/wake/pollOnce) from a bionic .so\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

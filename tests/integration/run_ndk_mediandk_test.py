# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; docs/ndk-scope.md. No GPL-3.0 sources.
#
# a4l's own libmediandk.so (src/ndk/bionic/acl_libmediandk.c), staged into the bionic sysroot:
# an app's native library that needs it does not load without one.
#
# A bionic .so with DT_NEEDED on libmediandk.so drives it:
#   - the AMEDIAFORMAT_KEY_* names it imports as data have MediaFormat's values
#   - AMediaFormat keeps an int, a string and a buffer by key and gives them back, the buffer
#     as its own copy
#   - a key that is absent, or holds another type, is not found
#   - setting a key again replaces the value, and may change its type
#   - AMediaFormat_toString names the keys and values
#   - AMediaCodec_createDecoderByType returns NULL, and a call on NULL is an error: no codec
#     stands behind AMediaCodec yet
#
# Needs a staged bionic runtime ($AOSP). SKIPs (exit 77) when absent.
#
# Usage: run_ndk_mediandk_test.py <acl-ndk-probe> <source_root>
import os
import subprocess
import sys
import tempfile

SKIP = 77


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_ndk_mediandk_test.py <acl-ndk-probe> <source_root>\n")
        return 2
    probe, src_root = sys.argv[1], sys.argv[2]
    aosp = os.environ.get("AOSP", "/mnt/dev/aosp")
    linker = os.path.join(
        aosp, "out/target/product/generic_x86_64/apex/com.android.runtime/bin/linker64")
    if not os.path.exists(linker):
        sys.stdout.write(f"SKIP: no built bionic linker under {aosp}\n")
        return SKIP

    work = tempfile.mkdtemp(prefix="a4l-ndk-mediandk-")
    try:
        tools = os.path.join(src_root, "tools")
        env = dict(os.environ, AOSP=aosp)
        stage = os.path.join(work, "bionic-runtime")
        so_dir = os.path.join(work, "repro")
        try:
            subprocess.run([os.path.join(tools, "stage-bionic-runtime.sh"), stage],
                           check=True, capture_output=True, text=True, env=env)
            subprocess.run([os.path.join(tools, "build-ndk-repro.sh"), so_dir], check=True,
                           capture_output=True, text=True,
                           env=dict(env, ACL_BIONIC_SYSROOT=os.path.join(stage, "bionic")))
        except subprocess.CalledProcessError as e:
            sys.stdout.write(f"SKIP: cannot stage bionic ({os.path.basename(e.cmd[0])})\n")
            return SKIP
        target = os.path.join(so_dir, "libmediatest.so")
        if not os.path.isfile(target):
            sys.stderr.write("FAIL: the stage has no libmediandk.so, so no libmediatest.so\n")
            return 1

        env.update({
            "ACL_BIONIC_LINKER": os.path.join(stage, "linker64"),
            "ACL_BIONIC_SYSROOT": os.path.join(stage, "bionic"),
            "ACL_BIONIC_STUB": os.path.join(stage, "stub"),
        })
        p = subprocess.run([probe, target, "--mediaformat"],
                           env=env, capture_output=True, text=True, timeout=120)
        out = p.stdout + p.stderr
    finally:
        subprocess.run(["rm", "-rf", work], check=False)

    if p.returncode != 0 or "(PASS)" not in out:
        sys.stderr.write(f"FAIL: --mediaformat exit {p.returncode}\n--- output ---\n{out}\n")
        return 1
    if "AMediaCodec_createDecoderByType(audio/mp4a-latm): no codec" not in out:
        sys.stderr.write(f"FAIL: the codec request was not reported\n--- output ---\n{out}\n")
        return 1
    sys.stdout.write("OK: a4l's libmediandk.so keeps an AMediaFormat and reports that it has "
                     "no codec, from a bionic .so\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

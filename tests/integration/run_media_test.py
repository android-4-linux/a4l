# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §A01.3 (automotive media path, G1.5a),
# docs/framework-bootstrap.md Gate G2.e. No GPL-3.0 sources.
#
# Prove the automotive media entry point: media.apk's MusicService extends
# android.service.media.MediaBrowserService (a shim on the boot classpath). Hosted
# via `aclcxx run --service=...`, aclcxx must instantiate it, onCreate(), onGetRoot()
# for the browse root, then onLoadChildren() and enumerate the MediaItems the app
# built (via MediaDescription.Builder) and sendResult'd — in-process, no Views, no
# Binder. Exercises a null (const 0 -> aconst_null) argument and the Builder chain.
#
# Usage: run_media_test.py <aclcxx_binary> <media.apk> <framework_dir>
import subprocess
import sys

CHECKS = [
    "hosting MediaBrowserService com.aclcxx.hello.MusicService",
    "media browse root: aclcxx_root",
    "media children of 'aclcxx_root': 2 item(s)",
    "media item: Song One",
    "media item: Song Two",
]


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_media_test.py <aclcxx> <media.apk> <fw_dir>\n")
        return 2
    aclcxx, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    p = subprocess.run(
        [aclcxx, "run", apk, "--vm=jvm", "--framework=" + fw,
         "--service=com.aclcxx.hello.MusicService"],
        capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr

    ok = p.returncode == 0
    if not ok:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n")
    for c in CHECKS:
        if c not in out:
            sys.stderr.write(f"FAIL: missing '{c}'\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: MediaBrowserService hosted; onGetRoot + onLoadChildren walked\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

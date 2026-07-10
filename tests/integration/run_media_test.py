# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §A01.3 (automotive media path, G1.5a),
# docs/framework-bootstrap.md Gate G2.e. No GPL-3.0 sources.
#
# Prove the automotive media entry point: media.apk's MusicService extends
# android.service.media.MediaBrowserService (a shim on the boot classpath). Hosted
# via `aclcxx run --service=...`, aclcxx must instantiate it, onCreate(), then
# onGetRoot() and read the returned BrowserRoot's id — in-process, no Views, no
# Binder. Exercises a null (const 0 -> aconst_null) reference argument too.
#
# Usage: run_media_test.py <aclcxx_binary> <media.apk> <framework_dir>
import subprocess
import sys

HOSTING = "hosting MediaBrowserService com.aclcxx.hello.MusicService"
ROOT = "media browse root: aclcxx_root"


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

    ok = True
    if p.returncode != 0:
        sys.stderr.write(f"FAIL: exit {p.returncode}\n"); ok = False
    if HOSTING not in out:
        sys.stderr.write("FAIL: MediaBrowserService was not hosted\n"); ok = False
    if ROOT not in out:
        sys.stderr.write("FAIL: onGetRoot did not yield the expected browse root\n"); ok = False

    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    sys.stdout.write("OK: MediaBrowserService hosted; onCreate + onGetRoot walked\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §A01.3 (automotive media path, G1.5a),
# androidx MediaBrowserServiceCompat docs. No GPL-3.0 sources.
#
# Prove the androidx media entry point: media_compat.apk's MusicService extends
# androidx.media.MediaBrowserServiceCompat (the support-library base most real Auto
# media apps, e.g. Spotify, extend). `acl run --service=...` must recognize the
# androidx base (not just the framework MediaBrowserService), instantiate it, and walk
# onGetRoot -> onLoadChildren in-process (no Views, no Binder), enumerating the
# MediaItems the app built via MediaDescriptionCompat.Builder.
#
# Usage: run_media_compat_test.py <acl_binary> <media_compat.apk> <framework_dir>
import os
import subprocess
import sys

CHECKS = [
    "hosting MediaBrowserServiceCompat com.acl.hello.MusicService",  # androidx base recognized
    "media browse root: acl_root",
    "media children of 'acl_root': 2 item(s)",
    "media item: Song One",
    "media item: Song Two",
    "MediaBridge: browse snapshot root='acl_root' 2 item(s)",
]


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_media_compat_test.py <acl> <media_compat.apk> <fw_dir>\n")
        return 2
    acl, apk, fw = sys.argv[1], sys.argv[2], sys.argv[3]
    env = dict(os.environ)
    env["ACL_AUDIO"] = "null"
    p = subprocess.run(
        [acl, "run", apk, "--vm=jvm", "--framework=" + fw,
         "--service=com.acl.hello.MusicService"],
        env=env, capture_output=True, text=True, timeout=120)
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
    sys.stdout.write("OK: androidx MediaBrowserServiceCompat hosted; onGetRoot + onLoadChildren walked\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

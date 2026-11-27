# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; ADR-0009 C2. No GPL-3.0 sources.
#
# `a4l run --split` as a user meets it, without a VM or a display: the broker
# admits the APK, starts the app process on the admitted file, and reports how
# it ended. --vm=null makes the app process leave after the package layer, so
# this covers the spawn, the argument and environment handoff and the exit
# path. It runs --split=unconfined so that it does not depend on bubblewrap;
# confinement is run_confine_test.py. Frames and input across the boundary
# are in tests/unit/split_present_test.cpp; pixels need a GPU and host ART
# and are not in CI.
#
# Usage: run_split_test.py <a4l_binary> <apk_dir> <1 if the build has the GPU
#        present path, else 0>
import os
import shutil
import subprocess
import sys
import tempfile

OK, UNAVAILABLE, REFUSED = 0, 2, 3


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: run_split_test.py <a4l> <apk_dir> <0|1>\n")
        return 2
    a4l, apk_dir, has_gpu = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
    signed = os.path.join(apk_dir, "ok-v3.apk")
    broken = os.path.join(apk_dir, "bad-entry-byte.apk")
    unsigned = os.path.join(apk_dir, "unsigned.apk")

    scratch = tempfile.mkdtemp(prefix="a4l-split-")
    failures = []

    def run(*args, env=None):
        full_env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("ACL_APK_", "ACL_SPLIT_"))}
        full_env["XDG_CACHE_HOME"] = os.path.join(scratch, "cache")
        full_env.update(env or {})
        p = subprocess.run([a4l, *args], env=full_env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60)
        return p.returncode, p.stdout

    def check(ok, what, out=""):
        if not ok:
            failures.append(what + ("\n" + out if out else ""))

    try:
        if not has_gpu:
            code, out = run("run", signed, "--split=unconfined", "--vm=null")
            check(code == UNAVAILABLE, "--split without the GPU path: exit %d" % code, out)
            check("--split is unavailable" in out, "no 'unavailable' message", out)
        else:
            # Admitted: the broker verifies, the app process runs the copy.
            code, out = run("run", signed, "--split=unconfined", "--vm=null",
                            "--verify=require")
            check(code == OK, "split run of a signed APK: exit %d" % code, out)
            # The two processes share stderr and their lines interleave in
            # either order, so nothing here depends on which side of the
            # broker's announcement a line of the app process lands. The
            # broker ran with --verify=require, so the "not checked" line can
            # only be the app process's, and it is that process's first.
            check("this process is the broker" in out,
                  "the broker did not announce the app process", out)
            staged = out.find("running the verified copy")
            child = out.find("signature: not checked (--verify=off)")
            check(child >= 0, "the app process verified again", out)
            check(0 <= staged < child,
                  "the broker did not stage a verified copy first", out)
            after = out[child:] if child >= 0 else ""
            check("opened " in after and "/a4l/verified/" in after
                  and "base.apk" in after,
                  "the app process did not open the verified copy", out)
            check("app process ended (exited)" in out,
                  "the broker did not report the exit", out)

            # Refused: nothing is started.
            for apk, args, env in (
                    (broken, [], {}),
                    (unsigned, ["--verify=require"], {}),
                    (unsigned, [], {"ACL_APK_VERIFY": "require"}),
                    (signed, ["--signer=" + "ab" * 32], {})):
                code, out = run("run", apk, "--split=unconfined", "--vm=null", *args,
                                env=env)
                check(code == REFUSED,
                      "refused %s %s: exit %d" % (os.path.basename(apk), args, code),
                      out)
                check("app process" not in out,
                      "an app process was started for a refused APK", out)

            # The app process's own failure is the broker's exit code.
            code, out = run("run", signed, "--split=unconfined", "--vm=nonesuch")
            check(code != OK, "a failing app process exited 0", out)

        # A stray ACL_SPLIT_FD is not adopted.
        code, out = run("run", signed, "--vm=null", env={"ACL_SPLIT_FD": "99"})
        check(code == REFUSED and "is not a socket" in out,
              "a bogus $ACL_SPLIT_FD: exit %d" % code, out)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        sys.stdout.write("FAIL:\n  " + "\n  ".join(failures) + "\n")
        return 1
    sys.stdout.write("OK: --split admits in the broker, runs the copy in the "
                     "app process, refuses before starting one\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

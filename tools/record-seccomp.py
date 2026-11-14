#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l tooling (ADR-0009 C4). Reads strace(1) output.
# No GPL-3.0 sources.
"""Record the system calls a confined --split app process makes.

The seccomp allowlist in src/confine/seccomp_allow_x86_64.inc is recorded,
not written by hand (ADR-0009, Follow-on). This tool does the recording:

  record-seccomp.py run   <trace-dir> <app.apk>...   trace each app
  record-seccomp.py list  <trace>...                 print the calls
  record-seccomp.py inc   <trace>...                 print the .inc body

`run` starts each APK through tools/run-apk.sh with --split and
ACL_SPLIT_SECCOMP=off under `strace -f`, and writes <trace-dir>/<apk>.strace.
It needs ACL_ART_HOME and a Wayland compositor, as run-apk.sh does.

`list` and `inc` take the app process out of a trace: the process that
executed a4l with --verify=off and every thread and process below it, from
the line where it reports itself confined, which is the point at which the
filter is installed in a normal run. The broker, bubblewrap and a4l's own
start are not the app process and are left out.

The output is a starting point. Review it before it is committed: a call
recorded here is one the corpus made, not one an app should have.
"""
import os
import re
import subprocess
import sys

LINE = re.compile(r"^(\d+)\s+(?:<\.\.\. )?(\w+)(?:\(| resumed>)")
CREATED = re.compile(r"^(\d+)\s+.*\b(?:clone3?|fork|vfork)\b.*\)\s+=\s+(\d+)\s*$")
MARKER = "app process confined"


def app_calls(path):
    """{syscall: count} for the app process tree of one trace.

    The app process is in its own pid namespace, so the number a clone
    returns inside it is not the pid strace prefixes the new thread's lines
    with, and the tree cannot be followed from the inside. It is found from
    the outside instead: the broker, its threads and bubblewrap are followed
    from the first process in the trace, whose clones return real pids, and
    everything else is the app process tree.
    """
    outside = set()
    root = None
    armed = False
    calls = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            pid, name = m.group(1), m.group(2)
            if not outside:
                outside.add(pid)
            if pid in outside:
                c = CREATED.match(line)
                if c:
                    outside.add(c.group(2))
                continue
            if root is None:
                if name == "execve" and "--verify=off" in line:
                    root = pid
                continue
            if not armed:
                armed = pid == root and name == "write" and MARKER in line
                continue
            calls[name] = calls.get(name, 0) + 1
    if not armed:
        sys.stderr.write("%s: no confined app process in this trace\n" % path)
    return calls


def merged(paths):
    total, apps = {}, {}
    for p in paths:
        for name, n in app_calls(p).items():
            total[name] = total.get(name, 0) + n
            apps[name] = apps.get(name, 0) + 1
    return total, apps


def cmd_run(args):
    if len(args) < 2:
        return usage()
    out_dir, apks = args[0], args[1:]
    os.makedirs(out_dir, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    for apk in apks:
        trace = os.path.join(
            os.path.abspath(out_dir),
            os.path.splitext(os.path.basename(apk))[0] + ".strace")
        env = dict(os.environ)
        env["ACL_SPLIT_SECCOMP"] = "off"
        env["ACL_EXEC_WRAP"] = "strace -f -qq -s 96 -o " + trace
        env.setdefault("ACL_RUN_TIMEOUT", "60")
        p = subprocess.run(
            [os.path.join(here, "run-apk.sh"), apk, "--split", "--present"],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True)
        calls = app_calls(trace) if os.path.exists(trace) else {}
        sys.stderr.write("%s: exit %d, %d distinct call(s)\n"
                         % (apk, p.returncode, len(calls)))
    return 0


def cmd_list(args):
    total, apps = merged(args)
    for name in sorted(total):
        print("%-24s %9d call(s) in %d of %d trace(s)"
              % (name, total[name], apps[name], len(args)))
    return 0


def cmd_inc(args):
    total, _ = merged(args)
    for name in sorted(total):
        print("#ifdef SYS_%s\nACL_SYSCALL(%s)\n#endif" % (name, name))
    return 0


def usage():
    sys.stderr.write(__doc__)
    return 2


def main():
    if len(sys.argv) < 3:
        return usage()
    cmd = {"run": cmd_run, "list": cmd_list, "inc": cmd_inc}.get(sys.argv[1])
    return cmd(sys.argv[2:]) if cmd else usage()


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness. No GPL-3.0 sources.
#
# A desktop portal that owns its bus name and never answers does not hold up an app's start.
# a4l asks the Settings portal for the color scheme twice while an app starts, and sd-bus
# waits 25 s for an answer by default, so such a portal cost 50 s: seen on a board with no
# desktop, where the bus starts xdg-desktop-portal and it has no backend. The test runs a
# private session bus with a stand-in that owns org.freedesktop.portal.Desktop and reads
# nothing, and runs acl-appearance-probe on it: both queries must say "no answer", the first
# within a few seconds and the second at once.
#
# Needs dbus-daemon. Exits 77 (skip) without it.
#
# Usage: run_portal_timeout_test.py <acl-appearance-probe> <acl-silent-name-stub>
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from run_notify_test import skip, start_bus, stop

FIRST_LIMIT_MS = 6000   # the call's own limit is 2 s
SECOND_LIMIT_MS = 500   # a failed query is not made again


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_portal_timeout_test.py <acl-appearance-probe> "
                         "<acl-silent-name-stub>\n")
        return 2
    probe, stub_exe = sys.argv[1], sys.argv[2]
    work = tempfile.mkdtemp(prefix="a4l-portal-")
    bus = stub = None
    try:
        bus, address = start_bus(work)
        if bus is None:
            return skip(address)
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
        stub_log = os.path.join(work, "stub.log")
        with open(stub_log, "wb") as log:
            stub = subprocess.Popen([stub_exe, "org.freedesktop.portal.Desktop"], env=env,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        for _ in range(50):
            with open(stub_log) as fh:
                if "STUB: ready" in fh.read():
                    break
            if stub.poll() is not None:
                with open(stub_log) as fh:
                    sys.stderr.write(f"FAIL: the stand-in portal exited\n{fh.read()}\n")
                return 1
            time.sleep(0.1)
        try:
            p = subprocess.run([probe], env=env, capture_output=True, text=True, timeout=90)
        except subprocess.TimeoutExpired:
            sys.stderr.write("FAIL: the probe was still waiting after 90 s\n")
            return 1
        out = p.stdout + p.stderr
        got = re.findall(r"color-scheme query (\d): (-?\d+) in (\d+) ms", out)
        if p.returncode != 0 or len(got) != 2:
            sys.stderr.write(f"FAIL: probe exit {p.returncode}\n{out}\n")
            return 1
        (_, a1, t1), (_, a2, t2) = got
        if a1 != "-1" or a2 != "-1":
            sys.stderr.write(f"FAIL: a portal that never answers gave an answer\n{out}\n")
            return 1
        if int(t1) > FIRST_LIMIT_MS or int(t2) > SECOND_LIMIT_MS:
            sys.stderr.write(f"FAIL: the queries took {t1} ms and {t2} ms\n{out}\n")
            return 1
    finally:
        stop(stub)
        stop(bus)
        shutil.rmtree(work, ignore_errors=True)
    sys.stdout.write(f"OK: no answer from the portal cost {t1} ms, then {t2} ms\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness. No GPL-3.0 sources.
#
# A backend finds a service the bus has not started yet. On a system with no desktop UPower
# is installed and not running: the system bus starts it when it is first called. a4l used
# to ask only whether the name had an owner, and fell back to battery defaults there.
#
# The test runs a private bus that stands in for the system bus (DBUS_SYSTEM_BUS_ADDRESS),
# with a service file that lets it start a stand-in UPower (acl-upower-stub: one battery at
# 73 %, discharging), and runs acl-battery-probe on it. The probe must report the upower
# backend and that battery. On a second bus the stand-in is UPower on a machine with no
# battery, and the probe must report a full cell. A third has a service file whose program
# exits at once: the probe must report the null backend, and soon.
#
# Needs dbus-daemon. Exits 77 (skip) without it.
#
# Usage: run_bus_activation_test.py <acl-battery-probe> <acl-upower-stub>
import os
import shutil
import subprocess
import sys
import tempfile
import time

from run_notify_test import skip, stop

BUS_CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path={path}</listen>
  <servicedir>{services}</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""

SERVICE = """[D-BUS Service]
Name=org.freedesktop.UPower
Exec={program}
"""


def start_bus(work, tag, program, **more_env):
    """A private bus that can start `program` for org.freedesktop.UPower."""
    exe = shutil.which("dbus-daemon")
    if not exe:
        return None, "dbus-daemon not found"
    services = os.path.join(work, "services-" + tag)
    os.makedirs(services)
    with open(os.path.join(services, "org.freedesktop.UPower.service"), "w") as fh:
        fh.write(SERVICE.format(program=program))
    config = os.path.join(work, f"bus-{tag}.conf")
    path = os.path.join(work, "bus-" + tag)
    with open(config, "w") as fh:
        fh.write(BUS_CONFIG.format(path=path, services=services))
    # What the bus starts inherits its environment, and the stand-in looks for the system
    # bus: point it back at this one.
    env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + path, **more_env)
    proc = subprocess.Popen([exe, f"--config-file={config}", "--nofork", "--print-address=1"],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, start_new_session=True)
    address = proc.stdout.readline().strip()
    if not address:
        proc.kill()
        return None, "dbus-daemon did not start"
    return proc, address


def probe(exe, address):
    env = {k: v for k, v in os.environ.items() if k != "ACL_BATTERY_BACKEND"}
    env["DBUS_SYSTEM_BUS_ADDRESS"] = address
    start = time.monotonic()
    p = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr, time.monotonic() - start


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: run_bus_activation_test.py <acl-battery-probe> "
                         "<acl-upower-stub>\n")
        return 2
    probe_exe, stub_exe = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    work = tempfile.mkdtemp(prefix="a4l-activation-")
    good = bare = bad = None
    try:
        good, address = start_bus(work, "good", stub_exe)
        if good is None:
            return skip(address)
        rc, out, took = probe(probe_exe, address)
        if (rc != 0 or "acl battery backend: upower" not in out
                or "level        = 73/100" not in out):
            sys.stderr.write(f"FAIL: UPower the bus can start was not used (exit {rc})\n{out}\n")
            return 1

        # UPower on a machine with no battery: the app is told what it is told where there
        # is no UPower, a full cell, not a flat one.
        bare, address = start_bus(work, "bare", stub_exe, ACL_STUB_NO_BATTERY="1")
        if bare is None:
            return skip(address)
        rc, out, took = probe(probe_exe, address)
        if (rc != 0 or "acl battery backend: upower" not in out
                or "level        = 100/100" not in out or "present      = true" not in out):
            sys.stderr.write(f"FAIL: UPower with no battery (exit {rc})\n{out}\n")
            return 1

        # A service that cannot start: the bus reports the failure and the null backend is
        # used, without the wait a call's default timeout would be.
        bad, address = start_bus(work, "bad", shutil.which("false") or "/bin/false")
        if bad is None:
            return skip(address)
        rc, out, took = probe(probe_exe, address)
        if rc != 0 or "acl battery backend: null" not in out:
            sys.stderr.write(f"FAIL: with a UPower that cannot start (exit {rc})\n{out}\n")
            return 1
        if took > 10:
            sys.stderr.write(f"FAIL: a UPower that cannot start held the probe {took:.1f} s\n")
            return 1
    finally:
        # The good bus's child, the stand-in, goes with its session.
        for proc in (good, bare, bad):
            if proc is not None:
                try:
                    os.killpg(proc.pid, 15)
                except OSError:
                    pass
            stop(proc)
        shutil.rmtree(work, ignore_errors=True)
    sys.stdout.write("OK: a UPower the bus had to start was found and read; one that cannot "
                     "start gave battery defaults\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness. No GPL-3.0 sources.
#
# The keystore backend a4l picks against a real gnome-keyring. Started outside a desktop
# login the daemon has no default keyring and refuses every store, so a4l must fall back to
# its in-memory keystore, not report the Secret Service and lose each key. Given a password
# on standard input the daemon makes and unlocks a login keyring, and a4l must then use it:
# a blob stored there is read back the same. Locked again, with nothing that can prompt for
# its password, the keyring refuses stores and a4l must be back in memory, without a wait.
#
# Runs a private session bus and a gnome-keyring-daemon whose home, data and control
# directories are temporary, so the user's own keyring is not touched. Needs dbus-daemon,
# dbus-send and gnome-keyring-daemon. Exits 77 (skip) without them.
#
# Usage: run_keystore_keyring_test.py <acl-keystore-probe>
import os
import shutil
import subprocess
import sys
import tempfile
import time

from run_notify_test import skip, start_bus, stop


def start_keyring(exe, env, work, tag, password):
    """Start the daemon and wait for it to own org.freedesktop.secrets."""
    args = [exe, "--foreground", "--components=secrets",
            "--control-directory=" + os.path.join(work, "control-" + tag)]
    if password is not None:
        args.append("--unlock")
    proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, start_new_session=True)
    if password is not None:
        proc.stdin.write(password.encode())
    proc.stdin.close()
    for _ in range(100):
        r = subprocess.run(
            ["dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.DBus",
             "/org/freedesktop/DBus", "org.freedesktop.DBus.NameHasOwner",
             "string:org.freedesktop.secrets"], env=env, capture_output=True, text=True)
        if "boolean true" in r.stdout:
            return proc
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    stop(proc)
    return None


def probe(exe, env, alias):
    p = subprocess.run([exe, alias], env=env, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: run_keystore_keyring_test.py <acl-keystore-probe>\n")
        return 2
    probe_exe = sys.argv[1]
    keyring = shutil.which("gnome-keyring-daemon")
    if not keyring or not shutil.which("dbus-send"):
        return skip("gnome-keyring-daemon or dbus-send not found")
    work = tempfile.mkdtemp(prefix="a4l-keyring-")
    bus = daemon = None
    try:
        bus, address = start_bus(work)
        if bus is None:
            return skip(address)
        home = os.path.join(work, "home")
        os.makedirs(os.path.join(home, "data"))
        env = {k: v for k, v in os.environ.items()
               if k not in ("ACL_KEYSTORE_BACKEND", "GNOME_KEYRING_CONTROL")}
        env.update(DBUS_SESSION_BUS_ADDRESS=address, HOME=home,
                   XDG_DATA_HOME=os.path.join(home, "data"))

        # No default keyring: the daemon is there and stores nothing.
        daemon = start_keyring(keyring, env, work, "bare", None)
        if daemon is None:
            return skip("gnome-keyring-daemon did not come up on the private bus")
        rc, out = probe(probe_exe, env, "bare-key")
        if rc != 0 or "backend    : memory" not in out or "no default keyring" not in out:
            sys.stderr.write(f"FAIL: with no default keyring (exit {rc})\n{out}\n")
            return 1
        stop(daemon)
        daemon = None

        # A login keyring, made and unlocked from standard input.
        daemon = start_keyring(keyring, env, work, "login", "a4l-test")
        if daemon is None:
            sys.stderr.write("FAIL: gnome-keyring-daemon --unlock did not come up\n")
            return 1
        rc, out = probe(probe_exe, env, "kept-key")
        if rc != 0 or "backend    : secret-service" not in out or "store      : ok" not in out:
            sys.stderr.write(f"FAIL: with a login keyring (exit {rc})\n{out}\n")
            return 1

        # The same keyring, locked, with nothing on the bus that can prompt for its password.
        lock = subprocess.run(
            ["dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.secrets",
             "/org/freedesktop/secrets", "org.freedesktop.Secret.Service.Lock",
             "array:objpath:/org/freedesktop/secrets/collection/login"],
            env=env, capture_output=True, text=True)
        if lock.returncode != 0:
            sys.stderr.write(f"FAIL: could not lock the keyring\n{lock.stderr}\n")
            return 1
        start = time.monotonic()
        rc, out = probe(probe_exe, env, "locked-key")
        took = time.monotonic() - start
        if rc != 0 or "backend    : memory" not in out or "keyring is locked" not in out:
            sys.stderr.write(f"FAIL: with a locked keyring (exit {rc})\n{out}\n")
            return 1
        if took > 10:
            sys.stderr.write(f"FAIL: a locked keyring held the probe for {took:.1f} s\n")
            return 1
    finally:
        stop(daemon)
        stop(bus)
        shutil.rmtree(work, ignore_errors=True)
    sys.stdout.write("OK: in memory with no default keyring or a locked one; the Secret Service "
                     "with one unlocked\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

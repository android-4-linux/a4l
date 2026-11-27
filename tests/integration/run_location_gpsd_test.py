# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; the lines are in the form gpsd_json(5) documents,
# with made-up positions. No gpsd code. No GPL-3.0 sources.
#
# The gpsd location source, against a stand-in gpsd: a TCP server here that speaks the part
# of gpsd's protocol a4l uses. acl-location-probe runs with ACL_LOCATION=gpsd and ACL_GPSD
# at the stand-in, and must:
#   - send the ?WATCH command and take no position from a TPV report that has no fix;
#   - deliver a 3D fix that arrives split across two packets, with its height, speed and
#     bearing;
#   - reconnect when the daemon drops the connection, and deliver the two fixes the second
#     connection sends, a 2D one (no height) among them, past a SKY report between them.
# Then, with nothing listening at ACL_GPSD, the probe must say the source is gpsd, report
# no fix and end; it must not hang or crash.
#
# Usage: run_location_gpsd_test.py <acl-location-probe>
import socket
import subprocess
import sys
import threading
import time

WATCH = b'?WATCH={"enable":true,"json":true};'
VERSION = b'{"class":"VERSION","release":"3.25","rev":"3.25","proto_major":3,"proto_minor":15}\r\n'
DEVICES = (b'{"class":"DEVICES","devices":[{"class":"DEVICE","path":"/dev/ttyACM0",'
           b'"driver":"u-blox","activated":"2026-11-27T20:50:00.000Z"}]}\r\n')
WATCHING = (b'{"class":"WATCH","enable":true,"json":true,"nmea":false,"raw":0,"scaled":false,'
            b'"timing":false,"split24":false,"pps":false}\r\n')
NO_FIX = b'{"class":"TPV","device":"/dev/ttyACM0","mode":1,"time":"2026-11-27T20:50:40.000Z"}\r\n'
FIX_A = (b'{"class":"TPV","device":"/dev/ttyACM0","mode":3,"time":"2026-11-27T20:50:41.000Z",'
         b'"lat":48.117300,"lon":11.516670,"altHAE":545.4,"altMSL":499.6,"epx":4.2,"epy":6.1,'
         b'"track":84.4,"speed":11.52,"eph":7.5}\r\n')
SKY = (b'{"class":"SKY","device":"/dev/ttyACM0","hdop":1.2,"satellites":['
       b'{"PRN":5,"el":31.0,"az":86.0,"ss":41.0,"used":true},'
       b'{"PRN":13,"el":12.0,"az":310.0,"ss":20.0,"used":false}]}\r\n')
FIX_B = (b'{"class":"TPV","device":"/dev/ttyACM0","mode":2,"time":"2026-11-27T20:50:43.000Z",'
         b'"lat":48.117400,"lon":11.516800,"epx":12.0,"epy":9.0}\r\n')
FIX_C = (b'{"class":"TPV","device":"/dev/ttyACM0","mode":3,"time":"2026-11-27T20:50:44.000Z",'
         b'"lat":48.117500,"lon":11.516900,"altHAE":546.0,"eph":6.0,"speed":0.0,"track":0.0}\r\n')


class StandIn:
    """Two connections' worth of gpsd, then nothing more."""

    def __init__(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(2)
        self.listener.settimeout(20)
        self.port = self.listener.getsockname()[1]
        self.watched = []      # what each connection sent before it was answered
        self.error = None
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def greet(self, conn):
        conn.settimeout(10)
        conn.sendall(VERSION)
        got = b""
        while WATCH not in got:
            chunk = conn.recv(256)
            if not chunk:
                raise RuntimeError(f"closed before ?WATCH; got {got!r}")
            got += chunk
        self.watched.append(got)
        conn.sendall(DEVICES + WATCHING)

    def run(self):
        try:
            conn, _ = self.listener.accept()
            with conn:
                self.greet(conn)
                conn.sendall(NO_FIX)
                half = len(FIX_A) // 2
                conn.sendall(FIX_A[:half])   # one report in two packets
                time.sleep(0.2)
                conn.sendall(FIX_A[half:])
                time.sleep(0.3)
            conn, _ = self.listener.accept()  # the reconnection
            with conn:
                self.greet(conn)
                conn.sendall(FIX_B + SKY + FIX_C)
                self.done.wait(15)            # held open until the probe is finished
        except Exception as e:  # noqa: BLE001 - reported by the test
            self.error = e
        finally:
            self.listener.close()


def run_probe(probe, port, timeout):
    env = {"PATH": "/usr/bin:/bin", "ACL_LOCATION": "gpsd", "ACL_GPSD": f"127.0.0.1:{port}"}
    p = subprocess.run([probe], env=env, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: run_location_gpsd_test.py <acl-location-probe>\n")
        return 2
    probe = sys.argv[1]

    gpsd = StandIn()
    try:
        rc, out = run_probe(probe, gpsd.port, 40)
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: the probe did not end\n")
        return 1
    finally:
        gpsd.done.set()
    gpsd.thread.join(5)
    if gpsd.error is not None:
        sys.stderr.write(f"FAIL: stand-in gpsd: {gpsd.error}\n{out}\n")
        return 1
    fixes = [ln for ln in out.splitlines() if ln.startswith("fix #")]
    want = [
        ("fix #1: lat=48.117300 lon=11.516670 acc=7.5m t=1795812641000ms", "the split 3D fix"),
        ("fix #2: lat=48.117400 lon=11.516800 acc=12.0m (no-alt) (no-speed) (no-bearing) "
         "t=1795812643000ms", "the 2D fix after the reconnection"),
        ("fix #3: lat=48.117500 lon=11.516900 acc=6.0m t=1795812644000ms", "the last fix"),
    ]
    problems = []
    if "source=gpsd available=1 delivering=1" not in out:
        problems.append("the source is not gpsd")
    if rc != 0:
        problems.append(f"exit {rc}")
    if len(gpsd.watched) != 2:
        problems.append(f"{len(gpsd.watched)} connection(s) sent ?WATCH, not 2")
    if fixes != [w for w, _ in want]:
        problems.append("fixes are not the three sent, in order")
    if problems:
        sys.stderr.write("FAIL: " + "; ".join(problems) + f"\n--- probe ---\n{out}\n")
        return 1

    # Nothing listening where ACL_GPSD points: no fix, and the probe ends.
    free = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    free.bind(("127.0.0.1", 0))
    port = free.getsockname()[1]
    free.close()
    try:
        rc, out = run_probe(probe, port, 40)
    except subprocess.TimeoutExpired:
        sys.stderr.write("FAIL: with no gpsd the probe did not end\n")
        return 1
    if (rc != 1 or "source=gpsd" not in out or "done (0 fix(es))" not in out
            or "is not reachable" not in out):
        sys.stderr.write(f"FAIL: with no gpsd: exit {rc}\n{out}\n")
        return 1

    sys.stdout.write("OK: three fixes from a stand-in gpsd, one split across packets and two "
                     "after it dropped the connection; no gpsd is no fix\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

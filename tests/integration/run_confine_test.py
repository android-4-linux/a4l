# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; ADR-0009 C4. No GPL-3.0 sources.
#
# What a confined --split app process can reach, checked from the inside.
#
# Part one runs a shell probe in the sandbox a4l builds (the plan printed by
# confine_plan, handed to bubblewrap as a4l hands it) and asserts on what the
# probe could see and do: the home directory, the runtime directory with its
# display and bus sockets, other apps' data, the host's processes, the
# network, the GPU nodes, and where it can write.
#
# Part two runs `a4l run --split` itself with --vm=null: the app process
# reports itself confined after checking its own namespaces, descriptors and
# filter, and a refused bind stops the run before a child exists.
#
# Skips (77) where bubblewrap or unprivileged user namespaces are missing,
# unless $ACL_TEST_REQUIRE_CONFINE is set, which makes that a failure.
#
# Usage: run_confine_test.py <a4l> <confine_plan> <apk_dir> <1 if the build
#        has the GPU present path, else 0>
import os
import shutil
import socket
import subprocess
import sys
import tempfile

SKIP = 77
PROBE = r'''
say() { printf '%s=%s\n' "$1" "$2"; }
can_read() { if cat "$1" >/dev/null 2>&1; then echo yes; else echo no; fi; }
can_list() { if ls "$1" >/dev/null 2>&1; then echo yes; else echo no; fi; }
can_write() { if ( : > "$1/probe.$$" ) 2>/dev/null; then rm -f "$1/probe.$$"; echo yes; else echo no; fi; }
say secret "$(can_read "$T_HOME/secret.txt")"
say home_entries "$(ls -A "$T_HOME" 2>/dev/null | tr '\n' ' ')"
say runtime "$(can_list "$T_RUNTIME")"
say wayland "$(test -e "$T_RUNTIME/wayland-test" && echo yes || echo no)"
say bus "$(test -e "$T_RUNTIME/bus" && echo yes || echo no)"
say other_app "$(can_list "$T_DATA/acl/org.other.app")"
say other_verified "$(can_list "$T_CACHE/acl/verified")"
say apk "$(can_read "$T_APK")"
say apk_write "$( ( : >> "$T_APK" ) 2>/dev/null && echo yes || echo no)"
say data_write "$(can_write "$T_DATA/acl/$T_PKG")"
say cache_write "$(can_write "$T_CACHE/acl/$T_PKG")"
say tmp_write "$(can_write /tmp)"
say root_write "$(can_write /)"
say usr_write "$(can_write /usr)"
say etc_passwd "$(can_read /etc/passwd)"
say etc_shadow "$(test -e /etc/shadow && echo yes || echo no)"
say pid "$$"
say procs "$(ls /proc | grep -c '^[0-9][0-9]*$')"
say ifaces "$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' ' | tr '\n' ' ')"
say dri "$(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
say drm_class "$(ls /sys/class/drm 2>/dev/null | tr '\n' ' ')"
say drm_class_dev "$(for n in /sys/class/drm/*; do test -r "$n/dev" && printf '%s ' "${n##*/}"; done)"
say devs "$(ls /dev | tr '\n' ' ')"
say nnp "$(grep '^NoNewPrivs' /proc/self/status | tr -dc 0-9)"
say caps "$(grep '^CapEff' /proc/self/status | cut -f2)"
say env_names "$(env | cut -d= -f1 | tr '\n' ' ')"
say xdg_data "$XDG_DATA_HOME"
say android_data "$ANDROID_DATA"
say cwd "$(pwd)"
'''


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write(
            "usage: run_confine_test.py <a4l> <confine_plan> <apk_dir> <0|1>\n")
        return 2
    a4l, planner, apk_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    has_gpu = sys.argv[4] == "1"
    signed = os.path.join(apk_dir, "ok-v3.apk")
    pkg = "org.example.confined"

    # Not under /tmp: the sandbox has a /tmp of its own, and a host directory
    # there would be hidden for a reason that is not the one under test.
    base = os.environ.get("MESON_BUILD_ROOT") or os.path.dirname(
        os.path.abspath(planner))
    scratch = tempfile.mkdtemp(prefix="a4l-confine-", dir=base)
    home = os.path.join(scratch, "home")
    runtime = os.path.join(scratch, "runtime")
    data = os.path.join(home, ".local", "share")
    cache = os.path.join(home, ".cache")
    for d in (runtime, os.path.join(data, "acl", "org.other.app"),
              os.path.join(cache, "acl", "verified", "0000")):
        os.makedirs(d)
    with open(os.path.join(home, "secret.txt"), "w") as f:
        f.write("not for the app\n")
    # Bound through a descriptor of the directory: a socket address holds 108
    # bytes, and a build directory's path can be longer than that.
    listeners = []
    runtime_fd = os.open(runtime, os.O_RDONLY | os.O_DIRECTORY)
    for name in ("wayland-test", "bus"):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind("/proc/self/fd/%d/%s" % (runtime_fd, name))
        s.listen(1)
        listeners.append(s)
    os.close(runtime_fd)

    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("ACL_", "XDG_", "WAYLAND_", "DBUS_"))}
    env.update(HOME=home, XDG_RUNTIME_DIR=runtime,
               WAYLAND_DISPLAY="wayland-test",
               DBUS_SESSION_BUS_ADDRESS="unix:path=" + runtime + "/bus",
               SECRET_TOKEN="hunter2", ACL_PRESENT_GPU="1")
    failures = []

    def check(ok, what, detail=""):
        if not ok:
            failures.append(what + ("\n" + detail if detail else ""))

    def plan(*extra, more_env=None):
        e = dict(env)
        e.update(more_env or {})
        p = subprocess.run([planner, os.path.abspath(signed), pkg, *extra], env=e,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True, timeout=60)
        return p.returncode, p.stdout, p.stderr

    def probe(*extra):
        code, out, errtext = plan(*extra)
        if code != 0:
            return None, errtext
        lines = out.split("\n")
        cut = lines.index("--ENV--")
        argv = lines[:cut]
        child_env = dict(l.split("=", 1) for l in lines[cut + 1:] if "=" in l)
        child_env.update(T_HOME=home, T_RUNTIME=runtime, T_DATA=data,
                         T_CACHE=cache, T_APK=os.path.abspath(signed),
                         T_PKG=pkg)
        p = subprocess.run(argv + ["/usr/bin/sh", "-c", PROBE], env=child_env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60)
        got = dict(l.split("=", 1) for l in p.stdout.split("\n") if "=" in l)
        return got, p.stdout

    try:
        code, _, errtext = plan()
        if code == SKIP:
            # CI sets this so that a host that cannot confine fails the job
            # instead of passing it without having run the test.
            if os.environ.get("ACL_TEST_REQUIRE_CONFINE"):
                sys.stdout.write("FAIL: confinement is required here and "
                                 "unavailable: %s\n" % errtext.strip())
                return 1
            sys.stdout.write("SKIP: %s\n" % errtext.strip())
            return SKIP
        check(code == 0, "the plan failed: exit %d" % code, errtext)

        # --- part one: the sandbox, from the inside -----------------------
        got, raw = probe()
        if got is None or "pid" not in got:
            check(False, "the probe did not run", raw)
            got = {}
        want = {
            "secret": "no",           # a file in the home directory
            "home_entries": ".cache .local ",  # only the path to the app's dirs
            "runtime": "no",          # the runtime directory
            "wayland": "no",          # the compositor's socket
            "bus": "no",              # the session bus
            "other_app": "no",        # another app's data
            "other_verified": "no",   # other apps' verified copies
            "apk": "yes",             # this app's APK, read-only
            "apk_write": "no",
            "data_write": "yes",      # the app's own two directories
            "cache_write": "yes",
            "tmp_write": "yes",       # a private /tmp
            "root_write": "no",
            "usr_write": "no",
            "etc_passwd": "no",       # /etc is a short list, not the host's
            "etc_shadow": "no",
            "ifaces": "lo ",          # no network but loopback
            "nnp": "1",
            "caps": "0000000000000000",
            "cwd": "/",
            "xdg_data": data,
            "android_data": os.path.join(cache, "acl", pkg, "android-data"),
        }
        for key, value in want.items():
            check(got.get(key) == value,
                  "probe %s: got %r, want %r" % (key, got.get(key), value), raw)
        if got:
            # Its own pid namespace: the probe is one of a handful of
            # processes, not one of the host's.
            check(got.get("procs", "999").isdigit() and int(got["procs"]) <= 8,
                  "the probe sees %s processes" % got.get("procs"), raw)
            check(got.get("pid", "999").isdigit() and int(got["pid"]) <= 8,
                  "the probe's pid is %s" % got.get("pid"), raw)
            # GPU: render nodes only, never a card (modesetting) node.
            dri = got.get("dri", "").split()
            check(all(n.startswith("renderD") for n in dri),
                  "non-render DRM nodes in the sandbox: %s" % dri, raw)
            # Each has its entry in the DRM class, which is how udev finds
            # it (GStreamer's VA plugin lists devices that way), and the
            # class has nothing else: no card, no connector.
            check(got.get("drm_class", "").split() == dri and
                  got.get("drm_class_dev", "").split() == dri,
                  "the DRM class in the sandbox is %r (with a device number: "
                  "%r), the render nodes are %s" % (
                      got.get("drm_class"), got.get("drm_class_dev"), dri), raw)
            devs = set(got.get("devs", "").split())
            check(devs <= {"console", "core", "dri", "fd", "full", "null",
                           "ptmx", "pts", "random", "shm", "stderr", "stdin",
                           "stdout", "tty", "urandom", "zero"},
                  "unexpected /dev entries: %s" % sorted(devs), raw)
            names = set(got.get("env_names", "").split())
            for leaked in ("SECRET_TOKEN", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR",
                           "DBUS_SESSION_BUS_ADDRESS"):
                check(leaked not in names,
                      "$%s reached the app process" % leaked, raw)
            check("ACL_PRESENT_GPU" in names, "a4l's own variables are dropped",
                  raw)

        # --net: the host's interfaces are back, and nothing else changes.
        got_net, raw = probe("--net")
        if got_net is not None and "pid" in got_net:
            host_ifaces = set()
            with open("/proc/net/dev") as f:
                for line in f.readlines()[2:]:
                    host_ifaces.add(line.split(":")[0].strip())
            check(set(got_net.get("ifaces", "").split()) == host_ifaces,
                  "--net: interfaces %r" % got_net.get("ifaces"), raw)
            check(got_net.get("secret") == "no" and got_net.get("wayland") == "no",
                  "--net exposed more than the network", raw)
        else:
            check(False, "the --net probe did not run", raw)

        # Inputs that would undo the confinement are refused, not bound.
        for var, value in (("ACL_SPLIT_RO", home), ("ACL_ART_HOME", "/"),
                           ("ACL_SPLIT_RO", runtime),
                           ("ACL_FRAMEWORK", os.path.dirname(home) + "/../" +
                            os.path.basename(scratch) + "/home")):
            code, _, errtext = plan(more_env={var: value})
            check(code == 3 and "refusing" in errtext,
                  "%s=%s was not refused: exit %d" % (var, value, code), errtext)

        # --- part two: a4l itself ------------------------------------------
        def run(*args, more_env=None):
            # No display: the socket above is a name for the probe to look
            # for, not a compositor, and a4l would wait on it.
            e = {k: v for k, v in env.items() if k != "WAYLAND_DISPLAY"}
            e.update(more_env or {})
            p = subprocess.run([a4l, *args], env=e, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=60)
            return p.returncode, p.stdout

        if has_gpu:
            code, out = run("run", signed, "--split", "--vm=null",
                            "--verify=require")
            check(code == 0, "confined split run: exit %d" % code, out)
            check("confining the app process" in out,
                  "the broker did not announce the confinement", out)
            check("split: app process confined (system call filter" in out,
                  "the app process did not report itself confined", out)
            check("app process ended (exited)" in out,
                  "the broker did not report the exit", out)

            # A relative APK path still names the same file in the child.
            code, out = run("run", os.path.relpath(signed), "--split",
                            "--vm=null")
            check(code == 0 and "app process confined" in out,
                  "a relative APK path: exit %d" % code, out)

            # Unconfined is asked for by name and says what it is.
            code, out = run("run", signed, "--split=unconfined", "--vm=null")
            check(code == 0 and "nothing confines the app process" in out
                  and "app process confined" not in out,
                  "--split=unconfined: exit %d" % code, out)

            # Without the filter the app process says so, loudly.
            code, out = run("run", signed, "--split", "--vm=null",
                            more_env={"ACL_SPLIT_SECCOMP": "off"})
            check(code == 0 and "NO system call filter" in out,
                  "ACL_SPLIT_SECCOMP=off: exit %d" % code, out)

            # A bind that would expose the home: no child is started.
            code, out = run("run", signed, "--split", "--vm=null",
                            more_env={"ACL_SPLIT_RO": home})
            check(code == 2 and "cannot confine" in out
                  and "this process is the broker" not in out,
                  "a refused bind: exit %d" % code, out)

            # No bubblewrap: refused, with the way out named.
            code, out = run("run", signed, "--split", "--vm=null",
                            more_env={"ACL_BWRAP": "/nonexistent/bwrap"})
            check(code == 2 and "--split=unconfined" in out
                  and "this process is the broker" not in out,
                  "a missing bubblewrap: exit %d" % code, out)

            for bad in ("--split=sandboxed", "--net=lan"):
                code, out = run("run", signed, "--split", bad, "--vm=null")
                check(code == 1, "%s: exit %d" % (bad, code), out)
            code, out = run("run", signed, "--net=host", "--vm=null")
            check(code == 1 and "--net applies" in out,
                  "--net without --split: exit %d" % code, out)
            code, out = run("run", signed, "--split=unconfined", "--net=host",
                            "--vm=null")
            check(code == 1, "--net with --split=unconfined: exit %d" % code, out)
    finally:
        for s in listeners:
            s.close()
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        sys.stdout.write("FAIL:\n  " + "\n  ".join(failures) + "\n")
        return 1
    sys.stdout.write("OK: the sandbox hides the home, the runtime directory, "
                     "other apps and the network; the app process checks "
                     "itself\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

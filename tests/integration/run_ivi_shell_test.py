# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tests/integration/ivi_test_controller.c. No
# GPL-3.0 sources.
#
# a4l's window as an ivi surface (ivi-shell), on both presenters.
#
# A headless Weston is started with ivi-shell and a controller module built here
# (ivi_test_controller.c), which puts each ivi surface on a layer at 640x400. The present
# probes are run with ACL_WL_SHELL=ivi and a surface id, and the test asserts:
#   - the controller saw a surface created under that id and placed it;
#   - the probe presented its frames and the compositor answered its frame callbacks,
#     which it does only for a surface that is on a layer and painted.
# The dma-buf probe covers the GPU presenter and is run when it is built.
#
# Needs: Weston with ivi-shell.so, its development headers (libweston, ivi-layout-export.h)
# and a C compiler. Exits 77 (skip) where any is missing, which includes CI.
#
#   run_ivi_shell_test.py <wm_present_probe> [<wm_dmabuf_probe>]
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77
HERE = os.path.dirname(os.path.abspath(__file__))


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def pkg(prefix, *args):
    """pkg-config against the Weston under `prefix`, not whichever comes first on the
    default path: a controller built against another Weston's headers crashes this one."""
    env = dict(os.environ)
    env["PKG_CONFIG_LIBDIR"] = ":".join(
        os.path.join(prefix, d, "pkgconfig")
        for d in ("lib64", "lib", "lib/x86_64-linux-gnu", "lib/aarch64-linux-gnu", "share"))
    r = subprocess.run(["pkg-config", *args], env=env, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def build_controller(weston, work):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or not shutil.which("pkg-config"):
        return None, "no C compiler or pkg-config"
    prefix = os.path.dirname(os.path.dirname(os.path.realpath(weston)))
    version = pkg(prefix, "--modversion", "weston")
    if not version:
        return None, f"no Weston development files under {prefix}"
    libweston = f"libweston-{version.split('.')[0]}"
    cflags = pkg(prefix, "--cflags", "weston", libweston, "wayland-server", "pixman-1")
    if cflags is None:
        return None, f"no {libweston} development files under {prefix}"
    out = os.path.join(work, "a4l-ivi-ctl.so")
    last = ""
    # screen_ready is not in every Weston's ivi-layout interface: with it first.
    for extra in (["-DA4L_HAVE_SCREEN_READY"], []):
        r = subprocess.run([cc, "-shared", "-fPIC", "-o", out, *extra,
                            os.path.join(HERE, "ivi_test_controller.c"), *cflags.split()],
                           capture_output=True, text=True)
        if r.returncode == 0:
            return out, None
        last = r.stderr
    return None, "the controller does not compile:\n" + last


def start_weston(weston, module, runtime_dir, work, size):
    ini = os.path.join(work, "weston.ini")
    with open(ini, "w") as fh:
        fh.write(f"[core]\nshell=ivi-shell.so\nmodules={module}\n")
    env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "DISPLAY")}
    env["XDG_RUNTIME_DIR"] = runtime_dir
    env["A4L_IVI_SIZE"] = size
    log = open(os.path.join(work, "weston.log"), "w")
    sock = f"a4l-ivi-{os.getpid()}"
    p = subprocess.Popen([weston, "--backend=headless", "--renderer=gl", f"--socket={sock}",
                          "--width=1280", "--height=800", "-c", ini],
                         env=env, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(50):
        if os.path.exists(os.path.join(runtime_dir, sock)):
            return p, sock
        if p.poll() is not None:
            break
        time.sleep(0.1)
    p.kill()
    return None, open(os.path.join(work, "weston.log")).read()[-2000:]


def main() -> int:
    if len(sys.argv) not in (2, 3):
        sys.stderr.write("usage: run_ivi_shell_test.py <wm_present_probe> [<wm_dmabuf_probe>]\n")
        return 2
    weston = next((w for w in ("/usr/bin/weston", shutil.which("weston"))
                   if w and os.access(w, os.X_OK)), None)
    if not weston:
        return skip("weston is not installed")
    keep = os.environ.get("ACL_TEST_IVI_KEEP")
    work = keep or tempfile.mkdtemp(prefix="a4l-ivi-")
    os.makedirs(work, exist_ok=True)
    runtime_dir = os.path.join(work, "run")
    os.makedirs(runtime_dir, mode=0o700, exist_ok=True)
    failures = []
    try:
        module, why = build_controller(weston, work)
        if module is None:
            return skip(why)
        cases = [("shm", [sys.argv[1], "--frames", "30"], 7101)]
        if len(sys.argv) == 3:
            cases.append(("dmabuf", [sys.argv[2], "30"], 7102))
        for name, cmd, ivi_id in cases:
            cwork = os.path.join(work, name)
            os.makedirs(cwork, exist_ok=True)
            proc, sock = start_weston(weston, module, runtime_dir, cwork, "640x400")
            if proc is None:
                if "ivi-shell" in sock and "Failed to load" in sock:
                    return skip("this Weston has no ivi-shell", sock)
                return skip("weston did not start with ivi-shell", sock)
            try:
                env = dict(os.environ, XDG_RUNTIME_DIR=runtime_dir, WAYLAND_DISPLAY=sock,
                           ACL_WL_SHELL="ivi", ACL_IVI_SURFACE_ID=str(ivi_id))
                try:
                    r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=40)
                    out, rc = r.stdout + r.stderr, r.returncode
                except subprocess.TimeoutExpired as e:
                    out = (e.stdout or b"").decode(errors="replace") + \
                          (e.stderr or b"").decode(errors="replace")
                    rc = "timeout"
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
            with open(os.path.join(cwork, "probe.log"), "w") as fh:
                fh.write(out)
            wlog = open(os.path.join(cwork, "weston.log")).read()
            if name == "dmabuf" and "no Vulkan render target" in out:
                sys.stdout.write("dmabuf: skipped, no Vulkan render target here\n")
                continue

            def check(ok, what):
                if not ok:
                    failures.append(f"{name}: {what}")

            check(rc == 0, f"the probe exited {rc}")
            check(f"ivi_surface id={ivi_id}" in out, "a4l did not take the ivi role")
            check(f"a4l-ctl: surface {ivi_id} created" in wlog,
                  "the controller saw no surface under the id")
            check(f"a4l-ctl: surface {ivi_id} -> 640x400" in wlog,
                  "the controller did not place the surface")
            if name == "shm":
                m = re.search(r"(\d+) frame\(s\) committed, (\d+) shown", out)
                check(m is not None and int(m.group(2)) >= 25,
                      "the compositor answered too few frame callbacks")
                check("LOOP-PRESENTED-OK" in out, "the present loop did not finish clean")
            else:
                m = re.search(r"(\d+) frame\(s\), (\d+) callback\(s\); done \(clean\)", out)
                check(m is not None and int(m.group(2)) >= 25,
                      "the compositor answered too few frame callbacks")
            if any(f.startswith(name + ":") for f in failures):
                sys.stdout.write(f"--- {name} probe ---\n{out[-3000:]}\n"
                                 f"--- {name} weston ---\n{wlog[-1500:]}\n")
            else:
                sys.stdout.write(f"{name}: ivi surface {ivi_id} placed at 640x400, "
                                 f"{m.group(2)} frame callbacks\n")
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)
    if failures:
        sys.stdout.write("FAIL:\n  " + "\n  ".join(failures) + "\n")
        return 1
    sys.stdout.write("PASS\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

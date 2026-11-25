# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test harness; tools/desktop-entry.sh. No GPL-3.0 sources.
#
# tools/desktop-entry.sh writes and removes an APK's desktop entry. The APK here is a manifest
# alone, with a label that holds a newline and a file name that holds a space, a dollar sign
# and a percent sign. The checks: the entry's keys, with the label on one line and the launch
# command quoted as the Desktop Entry Specification asks; a second install replaces the first;
# remove deletes it, given the APK or the package alone; and a file at the entry's path that
# a4l did not write stops both install and remove and is left as it was.
#
# The icon is not covered: the fixture APK has no resources.
#
# Needs a host ART build (ACL_ART_HOME) and the AOSP toolchain. Exits 77 (skip) without them.
#
# Usage: run_desktop_entry_test.py <a4l>
import glob
import os
import shutil
import subprocess
import sys
import tempfile

SKIP = 77
PACKAGE = "com.acl.desktop"
LABEL = "Desk\nExec=/bin/false \\ top"
NAME = "Name=Desk Exec=/bin/false \\\\ top"
APK_NAME = "de mo$1%.apk"


def first(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def skip(why, out=""):
    sys.stdout.write(f"SKIP: {why}\n{out}")
    return SKIP


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: run_desktop_entry_test.py <a4l>\n")
        return 2
    acl = sys.argv[1]
    src_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        return skip("ACL_ART_HOME not set")
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    zipalign = first(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    if not zipalign:
        return skip("zipalign not found")

    script = os.path.join(src_root, "tools/desktop-entry.sh")
    fixtures = os.path.join(src_root, "tests/fixtures")
    work = tempfile.mkdtemp(prefix="a4l-desktop-")
    try:
        sys.path.insert(0, fixtures)
        from make_axml import build_axml  # noqa: E402
        app = os.path.join(work, "app")
        os.makedirs(app)
        with open(os.path.join(app, "AndroidManifest.xml"), "wb") as fh:
            fh.write(build_axml(utf8=True, package=PACKAGE, launcher=f"{PACKAGE}.Main",
                                label=LABEL))
        unaligned = os.path.join(work, "u.apk")
        apk = os.path.join(work, APK_NAME)
        subprocess.run(["zip", "-X", "-0", "-j", "-q", unaligned,
                        os.path.join(app, "AndroidManifest.xml")], check=True)
        subprocess.run([zipalign, "-f", "4", unaligned, apk], check=True)

        data = os.path.join(work, "data")
        entry = os.path.join(data, "applications", f"{PACKAGE}.desktop")
        env = dict(os.environ, ACL_BIN=acl, XDG_DATA_HOME=data, ACL_RUN_TIMEOUT="120",
                   ACL_PRESENT_GPU="1")
        env.pop("ACL_AUDIO", None)

        def step(action, *args, target=apk):
            p = subprocess.run([script, action, target, *args], env=env, capture_output=True,
                               text=True, timeout=200)
            return p.returncode, p.stdout + p.stderr

        def fail(why, out=""):
            story = "\n".join(ln for ln in out.splitlines() if "DESKTOP: " in ln)
            sys.stderr.write(f"FAIL: {why}\n{story}\n")
            return 1

        rc, out = step("install")
        if rc == SKIP:
            return skip("tools/run-apk.sh skipped", out)
        if rc != 0 or not os.path.isfile(entry):
            sys.stderr.write(out[-4000:])
            return fail(f"install exited {rc}", out)
        with open(entry, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
        quoted_apk = '"' + apk.replace("$", "\\\\$").replace("%", "%%") + '"'
        want = [
            "[Desktop Entry]",
            "Type=Application",
            NAME,
            'Exec="env" "ACL_ART_HOME=' + home + '" "ACL_RUN_TIMEOUT=0" "ACL_PRESENT_GPU=1" "'
            + os.path.join(src_root, "tools/run-apk.sh") + '" ' + quoted_apk + ' "--present"',
            f"StartupWMClass={PACKAGE}",
            "Terminal=false",
            f"X-A4L-Package={PACKAGE}",
        ]
        if lines != want:
            return fail("the entry is not the one expected:\n  " + "\n  ".join(lines)
                        + "\nexpected\n  " + "\n  ".join(want), out)
        if os.stat(entry).st_mode & 0o777 != 0o644:
            return fail(f"the entry's mode is {oct(os.stat(entry).st_mode & 0o777)}, not 0644")
        validate = shutil.which("desktop-file-validate")
        if validate:
            p = subprocess.run([validate, entry], capture_output=True, text=True)
            if p.returncode != 0:
                return fail("desktop-file-validate: " + p.stdout + p.stderr)

        # A second install, with other launch arguments, replaces the first.
        rc, out = step("install", "--present", "--verify=require")
        with open(entry, encoding="utf-8") as fh:
            again = fh.read()
        if rc != 0 or '"--present" "--verify=require"\n' not in again:
            return fail(f"the second install (exit {rc}) did not replace the entry", out)
        if os.listdir(os.path.dirname(entry)) != [f"{PACKAGE}.desktop"]:
            return fail("a temporary file was left beside the entry")

        rc, out = step("remove")
        if rc != 0 or os.path.exists(entry):
            return fail(f"remove exited {rc} and the entry "
                        + ("is still there" if os.path.exists(entry) else "is gone"), out)
        rc, out = step("remove")
        if rc != 0:
            return fail(f"remove with nothing installed exited {rc}", out)

        # By package, under the other name, with no APK: what is left once the APK is gone.
        rc, out = step("install")
        if rc != 0 or not os.path.isfile(entry):
            return fail(f"the third install exited {rc}", out)
        rc, out = step("uninstall", target=PACKAGE)
        if rc != 0 or os.path.exists(entry):
            return fail(f"uninstall by package exited {rc} and the entry "
                        + ("is still there" if os.path.exists(entry) else "is gone"), out)

        # Somebody else's file at the entry's path.
        theirs = "[Desktop Entry]\nType=Application\nName=Mine\nExec=true\n"
        with open(entry, "w") as fh:
            fh.write(theirs)
        for action, target in (("install", apk), ("remove", apk), ("remove", PACKAGE)):
            rc, out = step(action, target=target)
            with open(entry) as fh:
                now = fh.read()
            if rc == 0 or now != theirs:
                return fail(f"{action} over a file a4l did not write exited {rc}"
                            + ("" if now == theirs else " and changed it"), out)
        print("PASS: desktop entry installed, replaced, removed; another's file left alone")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

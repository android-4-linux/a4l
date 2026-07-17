# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/ndk-scope.md (ADR-5 / R-6);
# AOSP d8/zipalign host tooling. No GPL-3.0 sources.
#
# The bionic-NDK end-to-end on REAL host ART: an app's *bionic-ABI* native library, shipped in
# the APK as lib/x86_64/libndktest.so, is loaded in-process by acl's bionic loader and its JNI
# round-trips back into ART — driven the way a real app does it (System.loadLibrary from an
# Activity), not by the acl-ndk-probe harness.
#
# The chain under test:
#   acl run ndk.apk --vm=art
#     -> pkg extracts lib/x86_64/*.so -> $XDG_CACHE_HOME/acl/<pkg>/lib, exports ACL_BIONIC_APP_LIBS
#     -> ART boots; ACL_BIONIC_LINKER set => Runtime.nativeLoad is routed to acl (jvm_vm)
#     -> Main.<clinit> System.loadLibrary("ndktest") -> nativeLoad -> is_bionic_elf => true
#     -> in-process bionic linker dlopens it, JNI_OnLoad runs across the %fs boundary,
#        RegisterNatives binds answer()
#     -> acl binds the loading class's remaining natives BY NAME (ART can't: it has no handle on
#        a lib the bionic linker opened), skipping what JNI_OnLoad already registered
#     -> Main.onCreate calls answer() -> down-call trampoline -> bionic native
#        -> helper(20,30) varargs up-call into ART -> 4250
# So the asserted 4250 proves load + JNI_OnLoad + down-call + up-call, not just a load; and
# nativeAnswer()=42 proves the bind-by-name path. answer()=4250 (not 9999) additionally proves a
# JNI_OnLoad registration beats the .so's exported Java_..._answer decoy — see build-ndk-repro.sh.
#
# Needs a host ART build ($ACL_ART_HOME), the AOSP host toolchain (d8/zipalign/jdk) and a built
# bionic ($AOSP, for the linker/sysroot/stub + the target .so). SKIPs (exit 77) when any is
# absent, so CI stays green without them.
#
# Usage: run_ndk_art_test.py <acl> <source_root> <android.jar> <ndk.apk>
import glob
import os
import subprocess
import sys
import tempfile

SKIP = 77  # meson interprets exit 77 as "skipped"

# --worker mode: the fixture also calls the native from a NON-main thread — an ART thread bionic
# never created and knows nothing about. The trampoline attaches it on first use (a hand-built
# bionic TCB), so the native must simply WORK there, and the process must survive.
#
# What this really pins is the absence of the corruption this path used to cause. Before the
# per-thread work, the worker's native returned 42 quite happily and ART then died inside the GC —
# "Check failed: self == thread" — because the thread came back with the MAIN thread's glibc %fs,
# so Thread::Current() (a TLS read) named the wrong thread. So: the value must be right, the
# process must exit cleanly, and that GC check must never appear.
WORKER_CHECKS = [
    "attached thread",                 # the trampoline attached it lazily, on first use
    "WORKER: calling nativeAnswer() off the main thread",
    "WORKER: nativeAnswer()=42",       # the native ran correctly on its own TCB
]
WORKER_MUST_NOT = [
    "Check failed: self == thread",  # the ART corruption a shared/borrowed TCB used to cause
    "no bionic TCB",                 # nothing should be refused now
]

CHECKS = [
    "NDK bionic loader armed",   # the gated nativeLoad hook installed (bootstraps lazily)
    "loaded bionic native lib",   # routed as bionic (not glibc dlopen)
    # bind-by-name resolved the exported symbol ART could never have dlsym'd itself
    "bind-by-name: com.acl.ndktest.Main.nativeAnswer()I",
    # answer()=4250: load+OnLoad+down-call+up-call all ran (RegisterNatives path)
    # nativeAnswer()=42: the name-bound native ran (bind-by-name path)
    "Main.onCreate: bionic native answer()=4250 nativeAnswer()=42",
]


def first_executable(*paths):
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            if os.access(p, os.X_OK):
                return p
    return None


def main() -> int:
    argv = list(sys.argv[1:])
    worker = "--worker" in argv           # off-main-thread refusal mode; see WORKER_CHECKS
    if worker:
        argv.remove("--worker")
    if len(argv) != 4:
        sys.stderr.write(
            "usage: run_ndk_art_test.py <acl> <src_root> <android.jar> <ndk.apk> [--worker]\n")
        return 2
    acl, src_root, android_jar, ndk_apk = argv

    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        sys.stdout.write("SKIP: ACL_ART_HOME not set (host ART build required)\n")
        return SKIP
    if not android_jar or not os.path.isfile(android_jar):
        sys.stdout.write("SKIP: no staged android.jar (framework stub layer)\n")
        return SKIP

    # AOSP tree root is three levels above out/host/linux-x86; $AOSP may point elsewhere.
    root = os.path.abspath(os.environ.get("AOSP", os.path.join(home, "..", "..", "..")))
    d8 = os.path.join(home, "bin", "d8")
    # linux*-x86 only — prebuilts/build-tools also ships darwin-x86 (sorts first).
    zipalign = first_executable(os.path.join(root, "prebuilts/build-tools/linux*-x86/bin/zipalign"))
    javac = first_executable(
        os.path.join(root, "prebuilts/jdk/jdk11/linux-x86/bin/javac"),
        os.path.join(root, "prebuilts/jdk/jdk17/linux-x86/bin/javac"))
    for name, tool in (("d8", d8), ("zipalign", zipalign), ("javac", javac)):
        if not tool or not os.access(tool, os.X_OK):
            sys.stdout.write(f"SKIP: {name} not found under {root}\n")
            return SKIP

    def run(cmd, env=None, cwd=None):
        subprocess.run(cmd, check=True, capture_output=True, text=True, env=env, cwd=cwd)

    work = tempfile.mkdtemp(prefix="acl-art-ndk-")
    try:
        tools = os.path.join(src_root, "tools")
        tool_env = dict(os.environ, AOSP=root)
        # 1. the bionic runtime acl bootstraps (linker64 + sysroot + stub), and the app's
        #    bionic-ABI .so. Both need a built bionic in $AOSP -> SKIP, not FAIL, if missing.
        stage = os.path.join(work, "bionic-runtime")
        so_dir = os.path.join(work, "repro")
        try:
            run([os.path.join(tools, "stage-bionic-runtime.sh"), stage], env=tool_env)
            # The fixture links acl's libandroid.so, so build it against the runtime just staged.
            run([os.path.join(tools, "build-ndk-repro.sh"), so_dir],
                env=dict(tool_env, ACL_BIONIC_SYSROOT=os.path.join(stage, "bionic")))
        except subprocess.CalledProcessError as e:
            sys.stdout.write(f"SKIP: no built bionic in {root} ({os.path.basename(e.cmd[0])})\n")
            return SKIP

        # 2. framework shim dex + android.jar stub dex (acl's build compiles the shims with the
        #    host javac, whose class-file version android-13's d8 rejects -> recompile at
        #    --release 8). Same recipe as run_activity_art_test.py.
        shim_srcs = (glob.glob(os.path.join(src_root, "framework/shim/**/*.java"), recursive=True)
                     + glob.glob(os.path.join(src_root, "third_party/aosp/frameworks/**/*.java"),
                                 recursive=True))
        shim_cls = os.path.join(work, "shimclasses")
        os.makedirs(shim_cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", shim_cls] + shim_srcs)
        shim_dex = os.path.join(work, "shim")
        os.makedirs(shim_dex)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", shim_dex]
            + glob.glob(os.path.join(shim_cls, "**/*.class"), recursive=True))
        stub_dex = os.path.join(work, "stub")
        os.makedirs(stub_dex)
        run([d8, "--min-api", "24", "--output", stub_dex, android_jar])

        # 3. the app: Main.java -> dex, + ndk.apk's binary manifest, + the bionic .so under
        #    lib/x86_64/ (where pkg looks). STORED + zipalign 4: ART mmaps the dex.
        app_java = os.path.join(src_root, "tests/fixtures/art_ndk/Main.java")
        app_cls = os.path.join(work, "appclasses")
        os.makedirs(app_cls)
        run([javac, "--release", "8", "-cp", android_jar, "-d", app_cls, app_java])
        app = os.path.join(work, "app")
        os.makedirs(app)
        run([d8, "--min-api", "24", "--lib", android_jar, "--output", app]
            + glob.glob(os.path.join(app_cls, "**/*.class"), recursive=True))
        run(["unzip", "-o", ndk_apk, "AndroidManifest.xml", "-d", app])
        os.makedirs(os.path.join(app, "lib/x86_64"))
        run(["cp", os.path.join(so_dir, "libndktest.so"), os.path.join(app, "lib/x86_64/")])
        unaligned = os.path.join(work, "unaligned.apk")
        apk = os.path.join(work, "ndk_art.apk")
        # -j flattens the top-level entries; the second pass runs in `app` (no -j) so the .so
        # keeps its lib/x86_64/ path, which is what pkg's ABI selection matches on.
        run(["zip", "-X", "-0", "-j", "-q", unaligned,
             os.path.join(app, "AndroidManifest.xml"), os.path.join(app, "classes.dex")])
        run(["zip", "-X", "-0", "-q", unaligned, "lib/x86_64/libndktest.so"], cwd=app)
        run([zipalign, "-f", "4", unaligned, apk])

        # 4. run it on host ART with the bionic loader armed. ACL_BIONIC_APP_LIBS is NOT set
        #    here on purpose — acl must export it itself from the extracted lib dir.
        env = dict(os.environ)
        env["ACL_ART_FRAMEWORK"] = (os.path.join(shim_dex, "classes.dex") + ":"
                                    + os.path.join(stub_dex, "classes.dex"))
        env["ACL_BIONIC_LINKER"] = os.path.join(stage, "linker64")
        env["ACL_BIONIC_SYSROOT"] = os.path.join(stage, "bionic")
        env["ACL_BIONIC_STUB"] = os.path.join(stage, "stub")
        env["XDG_CACHE_HOME"] = os.path.join(work, "cache")  # keep lib extraction in the sandbox
        env.pop("ACL_BIONIC_APP_LIBS", None)
        if worker:
            env["ACL_NDK_TEST_WORKER"] = "1"  # the fixture also calls the native off-main-thread
        p = subprocess.run([acl, "run", apk, "--vm=art"],
                           env=env, capture_output=True, text=True, timeout=300)
        out = p.stdout + p.stderr
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"FAIL: fixture build step failed: {e.cmd}\n{e.stderr}\n")
        return 1
    finally:
        subprocess.run(["rm", "-rf", work], check=False)

    ok = True
    if worker:
        if p.returncode != 0:
            sys.stderr.write(f"FAIL: off-main-thread native call: acl exit {p.returncode}\n")
            ok = False
        for c in WORKER_CHECKS:
            if c not in out:
                sys.stderr.write(f"FAIL: missing '{c}'\n")
                ok = False
        for c in WORKER_MUST_NOT:
            if c in out:
                sys.stderr.write(f"FAIL: '{c}' present\n")
                ok = False
    else:
        if p.returncode != 0:
            sys.stderr.write(f"FAIL: acl run --vm=art exit {p.returncode}\n")
            ok = False
        for c in CHECKS:
            if c not in out:
                sys.stderr.write(f"FAIL: missing '{c}'\n")
                ok = False
    if not ok:
        sys.stderr.write("--- captured output ---\n" + out + "\n")
        return 1
    if worker:
        sys.stdout.write("OK: an ART thread attached itself to bionic on first native call; "
                         "nativeAnswer()=42 off the main thread, no TLS corruption\n")
    else:
        sys.stdout.write("OK: app's bionic lib/x86_64/*.so loaded in-process on host ART; "
                         "JNI round-trip answer()=4250 (RegisterNatives, and NOT the exported 9999 "
                         "decoy) + nativeAnswer()=42 (bound by name)\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

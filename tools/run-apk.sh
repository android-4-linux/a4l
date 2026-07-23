#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original run harness; plan.md §5 (Tier B), docs/framework-bootstrap.md (A1.1),
# docs/art-host-spike.md (M3); AOSP host toolchain (d8, javac) + framework-minus-apex boot jars
# (Apache-2.0). No GPL-3.0 sources.
#
# Run an unmodified APK on the real host-ART backend (--vm=art) with acl's vendored framework and
# the org.acl.internal.HostBootstrap runtime environment. This is the developer counterpart to the
# tests/integration/run_*_art_test.py harnesses; unlike them it is interactive (--present) and takes
# an arbitrary APK.
#
# THE KEY MECHANISM (docs/framework-bootstrap.md A3) — the boot-vs-app classpath split:
#   framework-minus-apex sits on the ART BOOT classpath (-Xbootclasspath). A boot-classpath class
#   can only resolve other boot-classpath classes, so acl's clean-room android.* shims (which the
#   framework references — android.net.ConnectivityManager, android.bluetooth.*, ...) MUST also be
#   on the boot classpath, not the app classpath, or every such reference is an Unresolved Reference
#   that becomes a bare NoClassDefFoundError when called (this silently broke Dialog.setView). So we
#   compile framework/acl_host once and split it into TWO dex jars:
#     - boot jar (aclshims):  android/**  +  org/acl/internal/{BluetoothBridge,NsdBridge}
#                             reference only framework classes + each other)  -> ACL_ART_BOOTCLASSPATH
#     - app  jar (hostboot):  the rest of org/acl/internal/**  (HostBootstrap/PresentDriver, which
#                             reference app-bundled androidx/Compose)          -> ACL_ART_HOST_BOOTSTRAP
#   HostBootstrap is still found by native FindClass because the system classloader delegates to boot.
#
# Usage:   tools/run-apk.sh <app.apk> [extra acl args, e.g. --present --main=...]
# Env:     ACL_ART_HOME   (required) host ART build out/host/linux-x86 (libart.so + apex/com.android.art)
#          ACL_BIN        acl binary            (default: <repo>/build/src/shell/acl)
#          ACL_RUN_TIMEOUT seconds              (default: 60; 0 disables the timeout)
#          ACL_APK        APK path if $1 omitted
# Exits 77 (meson "skip" convention) when the host-ART build or toolchain is absent.
set -u
# AOSP's environment sometimes shadows coreutils with shell functions; use the real binaries.
unset -f grep egrep fgrep sed awk ls find 2>/dev/null || true

SKIP=77
die_skip() { printf 'SKIP: %s\n' "$1" >&2; exit "$SKIP"; }

HOME_DIR="${ACL_ART_HOME:-}"
[ -n "$HOME_DIR" ] && [ -d "$HOME_DIR" ] || die_skip "ACL_ART_HOME not set to a host ART build (out/host/linux-x86)"
[ -f "$HOME_DIR/lib64/libart.so" ] || die_skip "no libart.so under ACL_ART_HOME ($HOME_DIR)"

# The AOSP tree root is three levels above out/host/linux-x86.
ROOT=$(cd "$HOME_DIR/../../.." && pwd)
# The repo root is two levels above this script (tools/run-apk.sh).
ACLROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ACL_BIN="${ACL_BIN:-$ACLROOT/build/src/shell/acl}"
[ -x "$ACL_BIN" ] || die_skip "acl binary not built at $ACL_BIN (meson compile -C build acl)"

APK="${1:-${ACL_APK:-}}"
[ -n "$APK" ] && [ -f "$APK" ] || { printf 'usage: %s <app.apk> [acl args...]\n' "$0" >&2; exit 2; }
shift 2>/dev/null || true

first() { for p in "$@"; do [ -e "$p" ] && { printf '%s\n' "$p"; return; }; done; }

D8="$HOME_DIR/bin/d8"
JAVAC=$(first "$ROOT"/prebuilts/jdk/jdk11/linux-x86/bin/javac "$ROOT"/prebuilts/jdk/jdk17/linux-x86/bin/javac)
[ -x "$D8" ] || die_skip "d8 not found at $D8"
[ -x "${JAVAC:-}" ] || die_skip "javac not found under $ROOT/prebuilts/jdk"

# Framework the acl_host shims compile against (real android.* to bind method signatures).
FWCLASSES=$(first "$ROOT"/out/target/common/obj/JAVA_LIBRARIES/framework-minus-apex_intermediates/classes.jar)
CORECLASSES=$(first "$ROOT"/out/host/common/obj/JAVA_LIBRARIES/core-libart-hostdex_intermediates/classes.jar)
[ -n "${FWCLASSES:-}" ] || die_skip "framework-minus-apex classes.jar not built (m framework-minus-apex)"

# Framework BOOT jars (PRODUCT_BOOT_JARS, system), in order; framework-minus-apex is the anchor.
BOOT=""
for m in framework-minus-apex framework-graphics ext telephony-common voip-common ims-common; do
  j=$(first "$ROOT"/out/target/product/*/obj/JAVA_LIBRARIES/${m}_intermediates/javalib.jar)
  [ -n "$j" ] && BOOT="${BOOT:+$BOOT:}$j"
done
[ -n "$BOOT" ] || die_skip "framework boot jars not built (m framework-minus-apex framework-graphics ...)"
FRES=$(first "$ROOT"/out/target/product/*/system/framework/framework-res.apk)

W=$(mktemp -d "${TMPDIR:-/tmp}/acl-run-apk-XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/hbcls" "$W/bootdex" "$W/appdex"

# Compile all of framework/acl_host once (against the real framework + core), at --release 8 so the
# host javac's newer class-file version is accepted by this ART's d8.
"$JAVAC" -cp "$FWCLASSES${CORECLASSES:+:$CORECLASSES}" --release 8 -d "$W/hbcls" \
  $(find "$ACLROOT/framework/acl_host" -name '*.java') 2>&1 | head

# Split the compiled classes (see header): framework-facing -> boot jar, the rest -> app jar.
BOOTCLS=$( { find "$W/hbcls/android" -name '*.class' 2>/dev/null
             find "$W/hbcls/org/acl/internal" -name 'BluetoothBridge*.class' 2>/dev/null; \
             find "$W/hbcls/org/acl/internal" -name 'NsdBridge*.class' 2>/dev/null; } )
APPCLS=$(find "$W/hbcls/org" -name '*.class' 2>/dev/null | grep -vE '/(BluetoothBridge|NsdBridge)')

$D8 --min-api 24 --output "$W/bootdex" $BOOTCLS 2>&1 | tail -1
$D8 --min-api 24 --output "$W/appdex"  $APPCLS  2>&1 | tail -1
( cd "$W/bootdex" && zip -X -0 -q "$W/aclshims.jar" classes*.dex )
( cd "$W/appdex"  && zip -X -0 -q "$W/hostboot.jar" classes*.dex )

# aclshims.jar joins the BOOT classpath so framework-minus-apex resolves the android.* shims.
BOOT="$BOOT:$W/aclshims.jar"

# The bionic bridge (ADR-5): an app's lib/<abi>/*.so are bionic-ABI, so they load only through
# acl's in-process bionic linker. Stage the runtime (linker + sysroot incl. acl's libandroid.so +
# libz + the PIE stub) and point ACL_BIONIC_* at it. acl's Runtime.nativeLoad hook is gated on
# ACL_BIONIC_LINKER, so without this a native app (React Native, Flutter, …) fails on the first
# System.loadLibrary. Best-effort: if a bionic runtime can't be staged (no built bionic in $AOSP),
# skip it — Java-only apps still run. See tools/stage-bionic-runtime.sh + docs/ndk-scope.md.
BIONIC_ARGS=()
if "$ACLROOT/tools/stage-bionic-runtime.sh" "$W/bionic-runtime" >/dev/null 2>&1; then
  BIONIC_ARGS=(ACL_BIONIC_LINKER="$W/bionic-runtime/linker64"
               ACL_BIONIC_SYSROOT="$W/bionic-runtime/bionic"
               ACL_BIONIC_STUB="$W/bionic-runtime/stub")
else
  printf 'run-apk: no bionic runtime staged (no built bionic in $AOSP?); native libs will not load\n' >&2
fi

# Provision a CA trust store so app HTTPS works: acl points ANDROID_ROOT at $HOME_DIR, and AOSP
# conscrypt reads system CAs from $ANDROID_ROOT/etc/security/cacerts. Without it every TLS
# handshake fails ("Trust anchor for certification path not found"). Idempotent + best-effort.
"$ACLROOT/tools/gen-cacerts.sh" "$HOME_DIR/etc/security/cacerts" || \
  printf 'run-apk: CA trust store not provisioned; app HTTPS may fail\n' >&2

TIMEOUT="${ACL_RUN_TIMEOUT:-60}"
RUNNER=(env
  ACL_ART_BOOTCLASSPATH="$BOOT"
  ACL_ART_NATIVE=1
  "${BIONIC_ARGS[@]}"
  ACL_ART_HOST_BOOTSTRAP="$W/hostboot.jar")
# Where System.loadLibrary finds acl's supplemental native libs (libsoundpool.so etc.). ART's
# boot-classloader loader resolves via both java.library.path (ACL_ART_LIBPATH) and the dynamic
# linker path, so set both.
if [ -f "$ACLROOT/build/src/audio/libsoundpool.so" ]; then
  RUNNER+=(ACL_ART_LIBPATH="$ACLROOT/build/src/audio"
           LD_LIBRARY_PATH="$ACLROOT/build/src/audio:${LD_LIBRARY_PATH:-}")
fi
[ -n "${FRES:-}" ] && RUNNER+=(ACL_ART_FRAMEWORK_RES="$FRES")
RGFONT=$(first "$ROOT"/external/roboto-fonts/Roboto-Regular.ttf)
[ -n "${RGFONT:-}" ] && RUNNER+=(ACL_GRAPHICS_FONT="$RGFONT")
[ -f "$HOME_DIR/lib64/libacl_graphics_jni.so" ] && RUNNER+=(ACL_ART_GRAPHICS_LIB="$HOME_DIR/lib64/libacl_graphics_jni.so")
# android.opengl.GLES20/GLES30 natives (GLSurfaceView path); binds to host Mesa libGLESv2.
[ -f "$HOME_DIR/lib64/libacl_gles_jni.so" ] && RUNNER+=(ACL_ART_GLES_LIB="$HOME_DIR/lib64/libacl_gles_jni.so")
[ "$TIMEOUT" != "0" ] && RUNNER+=(timeout "$TIMEOUT")

# System fonts for the NATIVE (Flutter/Skia) engine. Skia's SkFontMgr_Android reads a HARDCODED
# /system/etc/fonts.xml and loads faces from $ANDROID_ROOT/fonts -- acl has neither, so a real
# app's default (sans-serif/Roboto) text renders blank. Provide both by running acl in a
# bubblewrap namespace that binds the AOSP target /system in place, plus the same fonts at
# $ANDROID_ROOT/fonts (= $HOME_DIR/fonts). No privilege needed; opt out with ACL_NO_SYSTEM=1,
# and it is skipped when bwrap or the font tree is absent (text then just falls back to missing).
SANDBOX=()
if [ -z "${ACL_NO_SYSTEM:-}" ] && command -v bwrap >/dev/null 2>&1; then
  SYSXML=$(first "$ROOT"/out/target/product/*/system/etc/fonts.xml)
  if [ -n "${SYSXML:-}" ]; then
    SYSDIR=$(cd "$(dirname "$SYSXML")/.." && pwd)   # .../<product>/system
    SANDBOX=(bwrap --dev-bind /dev /dev --proc /proc)
    # Optional kernel-level app-uid remap (ACL_SANDBOX_UID): a uid-1000 host user collides with
    # Android SYSTEM_UID, so Process.myUid()==SYSTEM_UID gates (WebViewFactory, ...) misfire. bwrap's
    # user namespace maps the host uid to a normal app uid, so getuid() is that value natively —
    # consistent for both framework identity and file ownership (the Java AclOs wrap is the fallback
    # for non-bwrap runs). e.g. ACL_SANDBOX_UID=10000.
    [ -n "${ACL_SANDBOX_UID:-}" ] && SANDBOX+=(--uid "$ACL_SANDBOX_UID" --gid "$ACL_SANDBOX_UID")
    for d in /*; do
      case "$d" in /proc|/dev|/system) continue ;; esac
      if [ -L "$d" ]; then SANDBOX+=(--symlink "$(readlink "$d")" "$d")
      elif [ -d "$d" ]; then SANDBOX+=(--bind "$d" "$d"); fi
    done
    # The engine's Skia (SkFontMgr_Android) resolves face filenames from fonts.xml against its
    # compiled-in SK_FONT_FILE_PREFIX, which in this engine build is "/fonts/" (root-relative), not
    # /system/fonts or $ANDROID_ROOT/fonts. Bind the AOSP fonts at all three so the faces resolve.
    SANDBOX+=(--ro-bind "$SYSDIR" /system \
              --ro-bind "$SYSDIR/fonts" "$HOME_DIR/fonts" \
              --ro-bind "$SYSDIR/fonts" /fonts)
    # The AOSP target's own fonts.xml is version 23 and maps sans-serif to a VARIABLE Roboto that
    # the engine's Skia yields no glyphs from (all default text renders blank). Override it with a
    # minimal config pointing at the STATIC faces in the same fonts dir. Opt out with ACL_AOSP_FONTS=1.
    FONTS_MIN="$ACLROOT/tools/aosp/fonts-min.xml"
    if [ -z "${ACL_AOSP_FONTS:-}" ] && [ -f "$FONTS_MIN" ]; then
      SANDBOX+=(--ro-bind "$FONTS_MIN" /system/etc/fonts.xml)
    fi
    # Dart's HttpClient (Flutter NetworkImage, dart:io TLS) loads its trusted roots on Android from
    # /system/etc/security/cacerts, which the AOSP target ships empty — so every HTTPS fetch fails
    # its cert check (network images never load). Bind the CA store gen-cacerts.sh already built
    # (hashed <subject>.0 PEMs) into that path. Opt out with ACL_NO_CACERTS=1.
    CACERTS="$HOME_DIR/etc/security/cacerts"
    if [ -z "${ACL_NO_CACERTS:-}" ] && [ -d "$CACERTS" ] && [ -n "$(ls -A "$CACERTS" 2>/dev/null)" ]; then
      # /system is bound read-only and the AOSP target has no .../security/cacerts dir, so bwrap
      # can't create that mountpoint under it. Overlay a writable tmpfs on the existing
      # /system/etc/security (its only contents, otacerts.zip + fsverity, aren't needed here) and
      # bind the CA store into it.
      SANDBOX+=(--tmpfs /system/etc/security --ro-bind "$CACERTS" /system/etc/security/cacerts)
    fi
    printf 'run-apk: engine fonts via bwrap (/system/etc/fonts.xml + %s/fonts)\n' "$HOME_DIR" >&2
    # WebView command-line file (ACL_WEBVIEW_ARGS): Chromium's CommandLineUtil reads
    # /data/local/tmp/webview-command-line when the app is debuggable (HostBootstrap sets
    # FLAG_DEBUGGABLE under ACL_WEBVIEW_ARGS). The only way to pass Chromium flags (--disable-gpu,
    # --enable-logging=stderr) on the obfuscated release WebView APK. First token is argv[0] (ignored).
    # ACL_WEBVIEW_VULKAN drives the hardware AwDrawFn Vulkan functor (viz + functor share acl's
    # Ganesh-Vulkan device, compositing straight into the present frame — no AHardwareBuffer copy).
    # That needs two Chromium switches: --webview-draw-functor-uses-vulkan (functor uses Vulkan) and
    # --disable-vulkan-surface (viz's GPU-init Vulkan runs headless — acl has no Android WSI surface).
    # Auto-append them so ACL_WEBVIEW_VULKAN=1 is sufficient. NOTE: on a host with several Vulkan ICDs
    # installed, a non-matching ICD (e.g. intel on an AMD box) can crash the loader during Chromium's
    # vkCreateInstance — pin the loader to the working ICD via VK_DRIVER_FILES (it propagates into the
    # sandbox), e.g. VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json.
    WV_ARGS="${ACL_WEBVIEW_ARGS:-}"
    if [ -n "${ACL_WEBVIEW_VULKAN:-}" ]; then
      case "$WV_ARGS" in *webview-draw-functor-uses-vulkan*) ;; *) WV_ARGS="$WV_ARGS --webview-draw-functor-uses-vulkan" ;; esac
      case "$WV_ARGS" in *disable-vulkan-surface*) ;; *) WV_ARGS="$WV_ARGS --disable-vulkan-surface" ;; esac
      # Reduce the checkerboard/blank-tile churn during zoom/scroll (opt out with ACL_NO_TILE_TUNING):
      #   --enable-low-res-tiling      with GPU raster cc disables low-res tiles, so a raster burst it
      #                                can't keep up with shows blank; force the low-res fallback back on
      #                                so those regions fill low-res -> sharp instead of blank.
      #   --enable-gpu-rasterization   force GPU raster even if a conservative host-GPU capability check
      #                                would leave it on the slower path.
      # These are cosmetic only: the ~2s map-zoom checkerboard is network tile-fetch latency (Blink
      # resource load), NOT an acl throughput limit — proven by cc sitting fully idle (0 raster / 0
      # decode) for the whole ~2s stall while present runs at 240fps, and by 640x480 (1/8 the tiles)
      # taking the identical time. A --force-gpu-mem-available-mb budget bump did NOT help (1024==2048,
      # 4096 broke rendering). Low-res tiling just makes the wait fill low-res -> sharp instead of blank.
      if [ -z "${ACL_NO_TILE_TUNING:-}" ]; then
        case "$WV_ARGS" in *enable-low-res-tiling*) ;; *) WV_ARGS="$WV_ARGS --enable-low-res-tiling" ;; esac
        case "$WV_ARGS" in *enable-gpu-rasterization*) ;; *) WV_ARGS="$WV_ARGS --enable-gpu-rasterization" ;; esac
      fi
    fi
    if [ -n "$WV_ARGS" ]; then
      WVCL="$W/webview-command-line"
      printf '_ %s\n' "$WV_ARGS" > "$WVCL"
      SANDBOX+=(--tmpfs /data --dir /data/local/tmp --ro-bind "$WVCL" /data/local/tmp/webview-command-line)
      printf 'run-apk: WebView command line -> %s\n' "$WV_ARGS" >&2
    fi
  fi
fi

# Optional exec wrapper inserted just before the acl binary (inside the sandbox), e.g. for
# tracing: ACL_EXEC_WRAP="strace -f -e trace=openat -qq -o /tmp/x.strace". Unset in normal runs.
read -r -a EXEC_WRAP <<< "${ACL_EXEC_WRAP:-}"
"${SANDBOX[@]}" "${RUNNER[@]}" "${EXEC_WRAP[@]}" "$ACL_BIN" run "$APK" --vm=art "$@" 2>&1

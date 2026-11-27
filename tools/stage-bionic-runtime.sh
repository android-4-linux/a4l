#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling. Copies AOSP-built bionic artifacts (Apache-2.0/BSD) out of a
# local AOSP checkout and builds a stub exe from tools/ndk-loader-spike/stub.c. Grounds
# docs/ndk-scope.md (ADR-5 / R-6). No GPL-3.0 sources.
#
# Stage the bionic runtime `acl` needs to load an app's bionic-ABI lib/<abi>/*.so in-process:
#
#   <outdir>/linker64        the bionic dynamic linker (bootstrapped by src/ndk/bionic_runtime)
#   <outdir>/bionic/*.so     soname-named bionic libs (libc/libm/libdl/liblog/libc++/libz/ld-android)
#                            — the linker resolves an app lib's NEEDED from here
#   <outdir>/stub            a minimal bionic PIE exe handed to the linker as the "main
#                            executable" so linker_main() completes (its entry never runs)
#   <outdir>/bionic/libandroid.so   acl's own (src/ndk/bionic/acl_libandroid.c), NOT AOSP's: an
#                            app's native code links libandroid.so, and on a host the NDK APIs have
#                            to be acl's (ALooper is real; the rest announce themselves — see the
#                            source and docs/ndk-scope.md)
#
# The three ACL_BIONIC_* vars the loader reads (src/ndk/bionic_runtime.cpp BionicConfig::from_env)
# map 1:1 onto those paths; --print-env emits them ready to eval.
#
# Usage: tools/stage-bionic-runtime.sh [outdir] [--print-env]
#   AOSP=/path/to/aosp   AOSP checkout root (default /mnt/dev/aosp), must be built for x86_64
#   outdir               stage destination (default: build/bionic-runtime under the repo)
#
#   eval "$(tools/stage-bionic-runtime.sh build/bionic-runtime --print-env)"
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
AOSP="${AOSP:-/mnt/dev/aosp}"
OUT="${1:-$HERE/../build/bionic-runtime}"
[ "${1:-}" = "--print-env" ] && OUT="$HERE/../build/bionic-runtime"
PRINT_ENV=0
for a in "$@"; do [ "$a" = "--print-env" ] && PRINT_ENV=1; done
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

say() { [ "$PRINT_ENV" = 1 ] || echo "$@"; }

CLANG="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d/bin/clang"
INT="$AOSP/out/soong/.intermediates"

# ACL_BIONIC_ARCH=arm64: the runtime for an arm64 host, from the tree's linux_bionic_arm64
# outputs (bionic built for a Linux arm64 host; the tree has no Android arm64 target built).
# a4l's shim libraries are built for it too. libdl_android, ICU and libstdc++ are not staged.
if [ "${ACL_BIONIC_ARCH:-x86_64}" = "arm64" ]; then
  V=linux_bionic_arm64
  A_LINKER="$INT/bionic/linker/linker/$V/linker64"
  A_CRTB="$INT/bionic/libc/crtbegin_dynamic/$V/crtbegin_dynamic.o"
  A_CRTE="$INT/bionic/libc/crtend_android/$V/obj/bionic/libc/arch-common/bionic/crtend.o"
  A_LIBS=(bionic/libc/libc:libc.so bionic/libm/libm:libm.so bionic/libdl/libdl:libdl.so
          system/logging/liblog/liblog:liblog.so bionic/linker/ld-android:ld-android.so
          external/libcxx/libc++:libc++.so)
  for f in "$CLANG" "$A_LINKER" "$A_CRTB" "$A_CRTE"; do
    [ -e "$f" ] || { echo "MISSING: $f  (no $V outputs in \$AOSP)" >&2; exit 1; }
  done
  say "== staging bionic runtime (arm64) -> $OUT =="
  cp -f "$A_LINKER" "$OUT/linker64"
  SYS="$OUT/bionic"
  mkdir -p "$SYS"
  for l in "${A_LIBS[@]}"; do
    src="$INT/${l%%:*}/${V}_shared/${l##*:}"
    [ -e "$src" ] || { echo "MISSING: $src" >&2; exit 1; }
    cp -f "$src" "$SYS/${l##*:}"
  done
  "$CLANG" --target=aarch64-linux-android31 -fPIC -pie -nostdlib \
    -o "$OUT/stub" "$A_CRTB" "$HERE/ndk-loader-spike/stub.c" "$A_CRTE" \
    "$SYS/libc.so" "$SYS/libdl.so" -fuse-ld=lld
  # As for x86_64 below, where each is described: symbolic libz, then a4l's libraries, built
  # against bionic's own headers. libandroid.so first, the others link it. A host build names
  # the file, and its soname, libz-host.so; the linker finds it by the file name.
  A_LIBZ="$INT/external/zlib/libz/${V}_shared/libz-host.so"
  [ -e "$A_LIBZ" ] || { echo "MISSING: $A_LIBZ" >&2; exit 1; }
  cp -f "$A_LIBZ" "$SYS/libz.so"
  chmod u+w "$SYS/libz.so"
  "$HERE/elf-set-symbolic.sh" "$SYS/libz.so" || echo "stage-bionic-runtime: libz.so left non-symbolic" >&2
  A_INC=(-nostdlibinc -I"$AOSP/bionic/libc/include" -I"$AOSP/bionic/libc/kernel/uapi"
         -I"$AOSP/bionic/libc/kernel/uapi/asm-arm64" -I"$AOSP/bionic/libc/kernel/android/uapi"
         -I"$AOSP/system/logging/liblog/include" -I"$AOSP/frameworks/wilhelm/include"
         -I"$AOSP/out/soong/ndk/sysroot/usr/include")
  A_CRTB_SO="$INT/bionic/libc/crtbegin_so/$V/crtbegin_so.o"
  A_CRTE_SO="$INT/bionic/libc/crtend_so/$V/obj/bionic/libc/arch-common/bionic/crtend_so.o"
  # soname : source : extra flags and libraries. Hidden visibility where x86_64 has it.
  for spec in \
      "libandroid.so:acl_libandroid.c:-fvisibility=hidden $SYS/liblog.so $SYS/libdl.so" \
      "libEGL.so:acl_egl_fwd.c:" \
      "libGLESv2.so:acl_gles_fwd.c:$SYS/libEGL.so" \
      "libGLESv3.so:acl_gles_fwd.c:$SYS/libEGL.so" \
      "libGLESv1_CM.so:acl_gles_fwd.c:$SYS/libEGL.so" \
      ${ACL_WEBVIEW_VULKAN:+"libvulkan.so:acl_vulkan_fwd.c:$SYS/libEGL.so"} \
      "libjnigraphics.so:acl_libjnigraphics.c:" \
      "libmediandk.so:acl_libmediandk.c:" \
      "libaaudio.so:acl_libaaudio.c:$SYS/libandroid.so" \
      "libwebviewchromium_plat_support.so:acl_libwebviewchromium_plat_support.c:" \
      "libOpenSLES.so:acl_libopensles.c:-fvisibility=hidden $SYS/libandroid.so -Wl,--version-script=$HERE/../src/ndk/bionic/acl_libopensles.map" \
      "libacl_sysprops.so:acl_system_properties.c:-fvisibility=hidden -Wl,-z,global"; do
    so="${spec%%:*}"; rest="${spec#*:}"; src="${rest%%:*}"; extra="${rest#*:}"
    # -mno-outline-atomics: the atomics inline, not calls into compiler-rt, which is not linked.
    # shellcheck disable=SC2086  # $extra is a list
    "$CLANG" --target=aarch64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
      -mno-outline-atomics -O2 -Wl,-soname,"$so" -Wl,-z,now "${A_INC[@]}" -o "$SYS/$so" \
      "$A_CRTB_SO" "$HERE/../src/ndk/bionic/$src" "$A_CRTE_SO" "$SYS/libc.so" $extra -fuse-ld=lld
  done
  if [ "$PRINT_ENV" = 1 ]; then
    echo "export ACL_BIONIC_LINKER=$OUT/linker64"
    echo "export ACL_BIONIC_SYSROOT=$SYS"
    echo "export ACL_BIONIC_STUB=$OUT/stub"
    exit 0
  fi
  echo "== staged (arm64: linker, bionic libraries, a4l's shim libraries, stub) =="
  exit 0
fi
LINKER="$AOSP/out/target/product/generic_x86_64/apex/com.android.runtime/bin/linker64"
BLIBC="$INT/bionic/libc/libc/android_x86_64_shared/libc.so"
BLIBM="$INT/bionic/libm/libm/android_x86_64_shared/libm.so"
BLIBDL="$INT/bionic/libdl/libdl/android_x86_64_shared/libdl.so"
BLIBDLANDROID="$INT/bionic/libdl/libdl_android/android_x86_64_shared/libdl_android.so"
BLIBLOG="$INT/system/logging/liblog/liblog/android_x86_64_shared/liblog.so"
LDANDROID="$INT/bionic/linker/ld-android/android_x86_64_shared/ld-android.so"
BLIBCXX="$INT/external/libcxx/libc++/android_x86_64_shared/libc++.so"
BLIBZ="$INT/external/zlib/libz/android_x86_64_shared/libz.so"
CRTBEGIN="$INT/bionic/libc/crtbegin_dynamic/android_x86_64/crtbegin_dynamic.o"
CRTEND="$INT/bionic/libc/crtend_android/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend.o"

# libstdc++.so is NOT in the AOSP tree (AOSP builds libc++_shared.so; the legacy gnustl-era
# libstdc++.so lives only in the NDK sysroot). A few old-toolchain apps (libGDX games) still
# DT_NEEDED it purely for the 5 C++ ABI symbols (__cxa_guard_*/__cxa_pure_virtual/std::nothrow).
# Best-effort: take the real bionic libstdc++.so from an NDK if one is present, else skip.
# ACL_NDK_LIBSTDCXX overrides the path outright; ACL_NDK points at an NDK root to search.
BLIBSTDCXX="${ACL_NDK_LIBSTDCXX:-}"
if [ -z "$BLIBSTDCXX" ]; then
  for ndk in "${ACL_NDK:-}" "$HOME"/Android/Sdk/ndk/* /opt/android-ndk-*; do
    [ -n "$ndk" ] && [ -d "$ndk" ] || continue
    libdir="$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android"
    [ -d "$libdir" ] || continue
    # any api level ships the same 5 C++ ABI symbols; take the lowest (widest compat)
    cand="$(ls -1v "$libdir"/*/libstdc++.so 2>/dev/null | head -1)"
    [ -n "$cand" ] && { BLIBSTDCXX="$cand"; break; }
  done
fi

for f in "$CLANG" "$LINKER" "$BLIBC" "$BLIBM" "$BLIBDL" "$BLIBLOG" "$LDANDROID" "$BLIBCXX" \
         "$BLIBZ" "$CRTBEGIN" "$CRTEND"; do
  [ -e "$f" ] || { echo "MISSING: $f  (build bionic in \$AOSP, or set AOSP=)" >&2; exit 1; }
done

# 1. the linker itself. Copied (not symlinked) so the stage is self-contained/relocatable.
say "== staging bionic runtime -> $OUT =="
cp -f "$LINKER" "$OUT/linker64"

# 2. sysroot: soname-named copies, so an app lib's NEEDED (libc.so, liblog.so, …) resolves from
#    here via the linker's LD_LIBRARY_PATH (there is no host ld.config.txt).
SYS="$OUT/bionic"
mkdir -p "$SYS"
cp -f "$BLIBC" "$SYS/libc.so"
cp -f "$BLIBM" "$SYS/libm.so"
cp -f "$BLIBDL" "$SYS/libdl.so"
cp -f "$BLIBLOG" "$SYS/liblog.so"
cp -f "$LDANDROID" "$SYS/ld-android.so"
# libdl_android.so: a linker-provided bionic stub (android_dlopen_ext / namespace fns). In the target
# /system/lib64 it is only a symlink into /apex/com.android.runtime, which the run-apk bwrap sandbox
# does NOT bind, so it dangles — and a media lib that NEEDs it (YouTube's libelements.so ->
# /system/lib64/libstagefright.so -> libdl_android.so) fails to load. Stage the real .so by soname.
[ -f "$BLIBDLANDROID" ] && cp -f "$BLIBDLANDROID" "$SYS/libdl_android.so"
cp -f "$BLIBCXX" "$SYS/libc++.so"
cp -f "$BLIBZ" "$SYS/libz.so"    # real bionic zlib; app libs (crashlytics/webp) NEED it
# An app may carry a zlib of its own inside one of its libraries and export it. libz.so calls
# its own functions through the PLT (deflateInit2_ calls deflateReset), and with both in one
# linker namespace such a call went to the app's copy, whose stream state has another layout:
# WhatsApp crashed in the deflateReset of its libar-bundle3.so, called from the system
# deflateInit2_. Symbolic, libz.so binds its own symbols to itself.
chmod u+w "$SYS/libz.so"
"$HERE/elf-set-symbolic.sh" "$SYS/libz.so" || echo "stage-bionic-runtime: libz.so left non-symbolic" >&2
# Android ICU (libandroidicu.so + libicuuc/libicui18n): lives in the com.android.i18n APEX, not
# /system/lib64. /system/lib64/libmedia.so DT_NEEDEDs libandroidicu.so, so an app native lib that
# transitively pulls in libmedia (YouTube's libelements.so) fails to load with UnsatisfiedLinkError
# ("libandroidicu.so not found") — aborting Application.onCreate — without these. There is no host
# ld.config.txt to map the apex into the default namespace, so stage the three by soname (their other
# deps — libbase in /system/lib64, liblog/libc++ here — already resolve). Best-effort.
ICUDIR="$AOSP/out/target/product/generic_x86_64/apex/com.android.i18n/lib64"
for icu in libandroidicu.so libicuuc.so libicui18n.so; do
  [ -f "$ICUDIR/$icu" ] && cp -f "$ICUDIR/$icu" "$SYS/$icu"
done
if [ -n "$BLIBSTDCXX" ] && [ -e "$BLIBSTDCXX" ]; then
  cp -f "$BLIBSTDCXX" "$SYS/libstdc++.so"   # NDK gnustl-era C++ ABI stub; old libGDX games NEED it
  say "   libstdc++.so <- $BLIBSTDCXX"
else
  say "   libstdc++.so: no NDK found (set ACL_NDK or ACL_NDK_LIBSTDCXX) — libGDX apps stay blocked"
fi

# 2b. acl's libandroid.so. AOSP's is a device library (Binder, SurfaceFlinger, logd); on a host the
# NDK API belongs to acl, so this one is ours — compiled for bionic here rather than by meson,
# which builds only host code. Linked against bionic libc + liblog by full path so its NEEDED
# sonames are bionic's, exactly like an app's own .so.
#
# -nostdlibinc is not optional: this is bionic-target code, and without it the HOST's /usr/include
# supplies the headers. pthread_mutex_t and epoll_event are not the same shape in glibc and bionic,
# so it would compile and then corrupt memory at run time inside bionic libc. Every header must
# come from the same bionic these libs are linked against (same rule as tools/gen-bionic-offsets.sh).
BIONIC_INC=(
  -nostdlibinc
  -I"$AOSP/bionic/libc/include"
  -I"$AOSP/bionic/libc/kernel/uapi"
  -I"$AOSP/bionic/libc/kernel/uapi/asm-x86"
  -I"$AOSP/bionic/libc/kernel/android/uapi"
  -I"$AOSP/system/logging/liblog/include"
  # LAST (lowest priority): the NDK sysroot supplies the framework headers bionic's libc does not —
  # android/hardware_buffer.h + hardware_buffer_jni.h and jni.h for libandroid's AHardwareBuffer C
  # ABI (Flutter external textures). Placed after the bionic includes so libc headers still win.
  -I"$AOSP/out/soong/ndk/sysroot/usr/include"
)
"$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
  -fvisibility=hidden -O2 -Wl,-soname,libandroid.so \
  "${BIONIC_INC[@]}" \
  -o "$SYS/libandroid.so" \
  "$INT/bionic/libc/crtbegin_so/android_x86_64/crtbegin_so.o" \
  "$HERE/../src/ndk/bionic/acl_libandroid.c" \
  "$INT/bionic/libc/crtend_so/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend_so.o" \
  "$BLIBC" "$BLIBLOG" "$BLIBDL" -fuse-ld=lld

# 2c. bionic GL: libEGL.so / libGLESv2.so are acl's generated host-Mesa forwarders (each entrypoint
# %fs-swaps to a glibc %fs and calls host Mesa — see tools/gen-gl-forwarders.py and
# src/ndk/bionic_runtime.cpp install_gl_forwarders). libjnigraphics.so is a minimal AndroidBitmap
# shim so a lib that NEEDs it resolves. An app's native GL (libflutter.so) links these three.
# -mfsgsbase: the forwarders use rd/wrfsbase (the same unprivileged path make_downcall uses).
GLCRTB="$INT/bionic/libc/crtbegin_so/android_x86_64/crtbegin_so.o"
GLCRTE="$INT/bionic/libc/crtend_so/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend_so.o"
# -z now: eager PLT binding, so a forwarder's per-call `call acl_gl_pick_donor@PLT` never traps into
# the lazy resolver (which would clobber the live argument registers). libEGL.so owns the shared
# per-thread-donor helper; libGLESv2.so DT_NEEDED links it so its forwarders resolve acl_gl_pick_donor.
# libEGL FIRST so it exists for the libGLESv2 link.
# libvulkan.so is the Vulkan analog of the GL forwarders (bionic->host-Mesa; DT_NEEDED libEGL for the
# shared acl_gl_pick_donor). Staged only in WebView Vulkan-functor mode (ACL_WEBVIEW_VULKAN) so the
# default GL/software path is unchanged; acl's install wiring handles its absence gracefully.
VK_FWD=""
[ -n "${ACL_WEBVIEW_VULKAN:-}" ] && VK_FWD="libvulkan.so:acl_vulkan_fwd.c:$SYS/libEGL.so"
# libGLESv3.so / libGLESv1_CM.so are the SAME generated forwarder as libGLESv2.so (its symbol table
# covers the GLES2/3 core; nothing acl runs uses GLES1 fixed-function). A native GL lib that links
# -lGLESv3 (librive-android.so) or -lGLESv1_CM (YouTube's libgoogle3.so) DT_NEEDs those sonames;
# without an acl copy the bionic linker falls back to the real /system/lib64 one, which drags in
# internal AOSP libEGL C++ symbols (android::egl_get_connection / egl_get_string_for_current_context)
# acl's forwarder does not provide. Shadowing them here makes the app link acl's forwarder instead;
# the app's actual GL calls resolve globally from the GLESv2/3 forwarder regardless of which .so
# DT_NEEDed it.
for pair in "libEGL.so:acl_egl_fwd.c:" "libGLESv2.so:acl_gles_fwd.c:$SYS/libEGL.so" \
            "libGLESv3.so:acl_gles_fwd.c:$SYS/libEGL.so" \
            "libGLESv1_CM.so:acl_gles_fwd.c:$SYS/libEGL.so" \
            "libjnigraphics.so:acl_libjnigraphics.c:" \
            "libmediandk.so:acl_libmediandk.c:" \
            "libaaudio.so:acl_libaaudio.c:$SYS/libandroid.so" \
            "libwebviewchromium_plat_support.so:acl_libwebviewchromium_plat_support.c:" \
            ${VK_FWD:+"$VK_FWD"}; do
  gso="${pair%%:*}"; rest="${pair#*:}"; gsrc="${rest%%:*}"; gextra="${rest#*:}"
  "$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls -mfsgsbase \
    -O2 -Wl,-soname,"$gso" -Wl,-z,now "${BIONIC_INC[@]}" -o "$SYS/$gso" \
    "$GLCRTB" "$HERE/../src/ndk/bionic/$gsrc" "$GLCRTE" "$BLIBC" ${gextra:+"$gextra"} -fuse-ld=lld
done
say "   libEGL.so / libGLESv2.so: host-Mesa forwarders (per-thread donors); libjnigraphics.so: shim"

# 2c'. libOpenSLES.so (src/ndk/bionic/acl_libopensles.c): an OpenSL ES engine with an audio
# recorder over a4l's audio input, reached through libandroid.so. App native libs DT_NEEDED it
# (Telegram's libtmessages, WhatsApp's libwhatsapp) and fail to load without it. It used to be
# the NDK's link stub, whose slCreateEngine makes no engine. The version script gives its symbols
# the LIBOPENSLES version a library built against the NDK asks for. The SLES headers come from
# the tree, for a checkout with no NDK sysroot built.
"$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
  -fvisibility=hidden -O2 -Wl,-soname,libOpenSLES.so -Wl,-z,now \
  -Wl,--version-script="$HERE/../src/ndk/bionic/acl_libopensles.map" \
  "${BIONIC_INC[@]}" -I"$AOSP/frameworks/wilhelm/include" -o "$SYS/libOpenSLES.so" \
  "$GLCRTB" "$HERE/../src/ndk/bionic/acl_libopensles.c" "$GLCRTE" "$BLIBC" "$SYS/libandroid.so" \
  -fuse-ld=lld
say "   libOpenSLES.so: engine + audio recorder shim"

# 2d. System properties for native code (src/ndk/bionic/acl_system_properties.c). -z global: the
# bionic linker searches such a library ahead of a library's own dependencies, libc among them, so
# these __system_property_* are the ones every library loaded after it binds. An RTLD_GLOBAL dlopen
# alone does not do that.
"$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
  -fvisibility=hidden -O2 -Wl,-soname,libacl_sysprops.so -Wl,-z,global -Wl,-z,now \
  "${BIONIC_INC[@]}" -o "$SYS/libacl_sysprops.so" \
  "$GLCRTB" "$HERE/../src/ndk/bionic/acl_system_properties.c" "$GLCRTE" "$BLIBC" -fuse-ld=lld

# 3. the bionic PIE stub exe (must link + pull bionic libc; entry never runs).
"$CLANG" --target=x86_64-linux-android31 -fPIC -pie -nostdlib \
  -o "$OUT/stub" "$CRTBEGIN" "$HERE/ndk-loader-spike/stub.c" "$CRTEND" \
  "$BLIBC" "$BLIBDL" -fuse-ld=lld -Wl,-soname,stub -Wl,-e,main 2>/dev/null \
|| "$CLANG" --target=x86_64-linux-android31 -fPIC -pie -nostdlib \
     -o "$OUT/stub" "$CRTBEGIN" "$HERE/ndk-loader-spike/stub.c" "$CRTEND" \
     "$BLIBC" "$BLIBDL" -fuse-ld=lld

if [ "$PRINT_ENV" = 1 ]; then
  echo "export ACL_BIONIC_LINKER=$OUT/linker64"
  echo "export ACL_BIONIC_SYSROOT=$SYS"
  echo "export ACL_BIONIC_STUB=$OUT/stub"
  exit 0
fi

echo "== staged =="
echo "  ACL_BIONIC_LINKER=$OUT/linker64"
echo "  ACL_BIONIC_SYSROOT=$SYS"
echo "  ACL_BIONIC_STUB=$OUT/stub"
echo "(eval \"\$($0 $OUT --print-env)\" to export them)"

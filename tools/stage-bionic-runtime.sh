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
LINKER="$AOSP/out/target/product/generic_x86_64/apex/com.android.runtime/bin/linker64"
BLIBC="$INT/bionic/libc/libc/android_x86_64_shared/libc.so"
BLIBM="$INT/bionic/libm/libm/android_x86_64_shared/libm.so"
BLIBDL="$INT/bionic/libdl/libdl/android_x86_64_shared/libdl.so"
BLIBLOG="$INT/system/logging/liblog/liblog/android_x86_64_shared/liblog.so"
LDANDROID="$INT/bionic/linker/ld-android/android_x86_64_shared/ld-android.so"
BLIBCXX="$INT/external/libcxx/libc++/android_x86_64_shared/libc++.so"
BLIBZ="$INT/external/zlib/libz/android_x86_64_shared/libz.so"
CRTBEGIN="$INT/bionic/libc/crtbegin_dynamic/android_x86_64/crtbegin_dynamic.o"
CRTEND="$INT/bionic/libc/crtend_android/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend.o"

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
cp -f "$BLIBCXX" "$SYS/libc++.so"
cp -f "$BLIBZ" "$SYS/libz.so"    # real bionic zlib; app libs (crashlytics/webp) NEED it

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
)
"$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
  -fvisibility=hidden -O2 -Wl,-soname,libandroid.so \
  "${BIONIC_INC[@]}" \
  -o "$SYS/libandroid.so" \
  "$INT/bionic/libc/crtbegin_so/android_x86_64/crtbegin_so.o" \
  "$HERE/../src/ndk/bionic/acl_libandroid.c" \
  "$INT/bionic/libc/crtend_so/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend_so.o" \
  "$BLIBC" "$BLIBLOG" "$BLIBDL" -fuse-ld=lld

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

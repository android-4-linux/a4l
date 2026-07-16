#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling. Uses AOSP prebuilt clang + built bionic intermediates + the
# AOSP linker64 from a local checkout. Grounds docs/ndk-scope.md Stage 0b. No GPL-3.0 sources.
#
# Builds the in-process bionic-linker bootstrap spike:
#   1. a bionic-ABI target .so (libndktest.so, reusing tools/build-ndk-repro.sh)
#   2. a bionic sysroot dir (linker64 + libc/libm/libdl/liblog/ld-android.so)
#   3. a minimal bionic PIE stub exe
#   4. the glibc spike harness (bionic_bootstrap.cpp + spike_main.cpp), -fno-stack-protector
# Then: out/spike <linker64> <stub> <sysroot> <libndktest.so> <symbol>   → expect "42".
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
AOSP="${AOSP:-/mnt/dev/aosp}"
OUT="$HERE/out"
mkdir -p "$OUT"

CLANG="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d/bin/clang"
INT="$AOSP/out/soong/.intermediates"
LINKER="$AOSP/out/target/product/generic_x86_64/apex/com.android.runtime/bin/linker64"
BLIBC="$INT/bionic/libc/libc/android_x86_64_shared/libc.so"
BLIBM="$INT/bionic/libm/libm/android_x86_64_shared/libm.so"
BLIBDL="$INT/bionic/libdl/libdl/android_x86_64_shared/libdl.so"
BLIBLOG="$INT/system/logging/liblog/liblog/android_x86_64_shared/liblog.so"
LDANDROID="$INT/bionic/linker/ld-android/android_x86_64_shared/ld-android.so"
BLIBCXX="$INT/external/libcxx/libc++/android_x86_64_shared/libc++.so"
CRTBEGIN="$INT/bionic/libc/crtbegin_dynamic/android_x86_64/crtbegin_dynamic.o"
CRTEND="$INT/bionic/libc/crtend_android/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend.o"

for f in "$CLANG" "$LINKER" "$BLIBC" "$BLIBM" "$BLIBDL" "$BLIBLOG" "$LDANDROID" "$BLIBCXX" "$CRTBEGIN" "$CRTEND"; do
  [ -e "$f" ] || { echo "MISSING: $f  (build bionic in \$AOSP, or set AOSP=)" >&2; exit 1; }
done

# 1. target .so
echo "== [1/4] target .so =="
AOSP="$AOSP" "$HERE/../build-ndk-repro.sh" "$OUT/repro" >/dev/null
SO="$OUT/repro/libndktest.so"
SYM="Java_com_acl_ndktest_Main_nativeAnswer"

# 2. bionic sysroot (soname-named copies so NEEDED resolves via LD_LIBRARY_PATH)
echo "== [2/4] bionic sysroot =="
SYS="$OUT/bionic"
mkdir -p "$SYS"
cp -f "$BLIBC" "$SYS/libc.so"
cp -f "$BLIBM" "$SYS/libm.so"
cp -f "$BLIBDL" "$SYS/libdl.so"
cp -f "$BLIBLOG" "$SYS/liblog.so"
cp -f "$LDANDROID" "$SYS/ld-android.so"
cp -f "$BLIBCXX" "$SYS/libc++.so"

# 3. minimal bionic PIE stub exe (entry never runs; must link + pull bionic libc)
echo "== [3/4] bionic stub exe =="
"$CLANG" --target=x86_64-linux-android31 -fPIC -pie -nostdlib \
  -o "$OUT/stub" "$CRTBEGIN" "$HERE/stub.c" "$CRTEND" \
  "$BLIBC" "$BLIBDL" -fuse-ld=lld -Wl,-soname,stub -Wl,-e,main 2>/dev/null \
|| "$CLANG" --target=x86_64-linux-android31 -fPIC -pie -nostdlib \
     -o "$OUT/stub" "$CRTBEGIN" "$HERE/stub.c" "$CRTEND" "$BLIBC" "$BLIBDL" -fuse-ld=lld

# 4. glibc spike harness (system clang; -fno-stack-protector for the %fs-swap windows)
echo "== [4/4] spike harness =="
c++ -std=c++20 -O0 -g -fno-stack-protector -I"$HERE" \
  -o "$OUT/spike" "$HERE/bionic_bootstrap.cpp" "$HERE/spike_main.cpp"

# Java fixture for the real-JVM end-to-end (acl-ndk-probe --realvm): a class matching the .so's
# JNI_OnLoad (FindClass com/acl/ndktest/Main) with a native answer() bound via RegisterNatives and
# a helper() the native up-calls. Compiled with the SAME JDK the probe links (class-file version).
JH="$(java -XshowSettings:properties -version 2>&1 | awk -F'= ' '/java.home/{print $2; exit}')"
if [ -n "$JH" ] && [ -x "$JH/bin/javac" ]; then
  cat >"$OUT/Main.java" <<'EOJ'
package com.acl.ndktest;
public class Main {
  public static native int answer();          // bound by the bionic .so's JNI_OnLoad
  public static int helper(int a, int b) { return a + b; }  // up-called by the native
}
EOJ
  rm -rf "$OUT/classes"; mkdir -p "$OUT/classes"
  "$JH/bin/javac" -d "$OUT/classes" "$OUT/Main.java" \
    && echo "== java fixture: $OUT/classes (ACL_NDK_TEST_CLASSES) =="
fi

echo
echo "== run =="
set -x
"$OUT/spike" "$LINKER" "$OUT/stub" "$SYS" "$SO" "$SYM"

#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling. Uses AOSP prebuilt clang + built bionic intermediates from a
# local AOSP checkout (paths overridable via env). Grounds docs/ndk-scope.md. No GPL-3.0 sources.
#
# Builds a real x86_64-linux-android (bionic-ABI) NDK-style .so and proves the host two-libc gap:
# a glibc process cannot dlopen it (glibc ld.so can't satisfy the bionic NEEDED libc.so/liblog.so).
# This is the negative control for docs/ndk-scope.md Stage 0b: it's why the bionic linker must be
# bootstrapped in-process (ADR-5 / R-6), not called via glibc dlopen.
#
# Usage: tools/build-ndk-repro.sh [out_dir]
#   AOSP=/path/to/aosp   AOSP checkout root (default /mnt/dev/aosp)
#   out_dir              where to place libndktest.so + dltest (default: mktemp)
set -euo pipefail

AOSP="${AOSP:-/mnt/dev/aosp}"
OUT="${1:-$(mktemp -d /tmp/ndk-repro.XXXXXX)}"
mkdir -p "$OUT"

CLANG="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d/bin/clang"
INT="$AOSP/out/soong/.intermediates"
BIONIC_LIBC="$INT/bionic/libc/libc/android_x86_64_shared/libc.so"
BIONIC_LIBLOG="$INT/system/logging/liblog/liblog/android_x86_64_shared/liblog.so"
BIONIC_LIBDL="$INT/bionic/libdl/libdl/android_x86_64_shared/libdl.so"
CRTBEGIN="$INT/bionic/libc/crtbegin_so/android_x86_64/crtbegin_so.o"
CRTEND="$INT/bionic/libc/crtend_so/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend_so.o"
# Headers: the prebuilt NDK sysroot is often absent in an AOSP checkout; use the in-tree sources.
INC_JNI="$AOSP/libnativehelper/include_jni"           # jni.h
INC_LOG="$AOSP/system/logging/liblog/include"          # android/log.h

for f in "$CLANG" "$BIONIC_LIBC" "$BIONIC_LIBLOG" "$BIONIC_LIBDL" "$CRTBEGIN" "$CRTEND"; do
  [ -e "$f" ] || { echo "MISSING: $f  (build bionic in \$AOSP, or set AOSP=)" >&2; exit 1; }
done

# --- The bionic-ABI NDK library (what an app ships in lib/x86_64/) ---
cat >"$OUT/ndktest.c" <<'EOF'
// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike fixture (generated). No GPL-3.0 sources.
#include <jni.h>
#include <android/log.h>
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_nativeAnswer(JNIEnv* e, jclass c) {
  __android_log_print(ANDROID_LOG_INFO, "ndktest", "nativeAnswer called");
  return 42;
}
// Bidirectional %fs-boundary self-test. Writes a bionic thread-local (bionic's dynamic TLS, via
// %fs), calls back into glibc via cb (which must be trampolined to glibc %fs), then confirms the
// bionic thread-local survived the round trip. __thread avoids any libc header (the repro compiles
// against host headers). Expected via BionicRuntime::run_boundary_selftest with cb(x)=x*2: cb(x)+1000.
static __thread volatile int t_marker;
int acl_ndk_boundary(int (*cb)(int), int x) {
  t_marker = 7;                               // bionic TLS write (needs bionic %fs)
  int r = cb(x);                              // up-call into glibc (needs glibc %fs)
  return r + (t_marker == 7 ? 1000 : 0);      // bionic TLS intact after the round trip?
}
// A native bound via RegisterNatives (not name resolution) — the real JNI_OnLoad path.
static jint the_registered(JNIEnv* env, jclass clazz) {
  t_marker = 55;                              // bionic TLS under the down-call (bionic %fs)
  return 4200;
}
// Exercises the JNIEnv/JavaVM boundary: GetEnv (VM up-call) then GetVersion/FindClass/
// RegisterNatives (Env up-calls), each crossing bionic->glibc. Returns JNI_VERSION_1_6.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env = 0;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
  (void)(*env)->GetVersion(env);
  jclass c = (*env)->FindClass(env, "com/acl/ndktest/Main");
  JNINativeMethod m = { "answer", "()I", (void*)the_registered };
  if ((*env)->RegisterNatives(env, c, &m, 1) != JNI_OK) return -1;
  t_marker = 33;                              // bionic TLS after the up-calls returned
  return JNI_VERSION_1_6;
}
EOF

# Link against the real bionic libs by full path so NEEDED sonames are bionic (libc.so, not
# libc.so.6) and no glibc crt/loader leaks in. -nostdlib + explicit crt = a clean bionic .so.
# -fno-emulated-tls: use native ELF TLS (__tls_get_addr, provided by bionic) for __thread rather
# than clang's android-default emulated-TLS (__emutls_get_address, a compiler-rt symbol not in bionic).
"$CLANG" --target=x86_64-linux-android31 \
  -fPIC -shared -nostdlib -fno-emulated-tls -Wl,-soname,libndktest.so \
  -I"$INC_JNI" -I"$INC_LOG" \
  -o "$OUT/libndktest.so" \
  "$CRTBEGIN" "$OUT/ndktest.c" "$CRTEND" \
  "$BIONIC_LIBC" "$BIONIC_LIBLOG" "$BIONIC_LIBDL" \
  -fuse-ld=lld 2>/dev/null \
|| "$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
     -Wl,-soname,libndktest.so -I"$INC_JNI" -I"$INC_LOG" -o "$OUT/libndktest.so" \
     "$CRTBEGIN" "$OUT/ndktest.c" "$CRTEND" \
     "$BIONIC_LIBC" "$BIONIC_LIBLOG" "$BIONIC_LIBDL" -fuse-ld=lld

echo "== built $OUT/libndktest.so =="
readelf -d "$OUT/libndktest.so" | grep -E 'NEEDED|SONAME'
echo "== exported symbol =="
readelf -sW "$OUT/libndktest.so" | grep nativeAnswer | head -1

# --- A glibc probe that tries to dlopen it (the failure) ---
cat >"$OUT/dltest.c" <<'EOF'
// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike fixture (generated). No GPL-3.0 sources.
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char** argv) {
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }
  int (*fn)(void*, void*) = (int(*)(void*,void*))dlsym(h, "Java_com_acl_ndktest_Main_nativeAnswer");
  printf("loaded; nativeAnswer=%d\n", fn ? fn(0,0) : -1);
  return 0;
}
EOF
cc -o "$OUT/dltest" "$OUT/dltest.c" -ldl   # host glibc cc

echo "== glibc dlopen of the bionic .so =="
"$OUT/dltest" "$OUT/libndktest.so" || true
echo "(expected: 'dlopen FAILED: .../libc.so: invalid ELF header' — the host NDK loader gap)"
echo "artifacts in $OUT"

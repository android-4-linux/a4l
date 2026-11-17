#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l build tooling. Builds LLVM compiler-rt pieces (Apache-2.0 with LLVM
# exception) with AOSP's prebuilt clang against a glibc sysroot. No GPL-3.0 sources.
#
# The clang prebuilt ships compiler-rt for x86 hosts and for Android, not for aarch64-linux-gnu.
# Soong links builtins and the minimal UBSan runtime into sanitized host modules, and ART's host
# build refers to the profile runtime, so those three are built here.
#
# Usage:  build-runtimes.sh [OUT_DIR]     default: $AOSP_ROOT/../glibc-arm64-runtimes
# Env:    ACL_AOSP_ROOT      default /mnt/dev/aosp
#         ACL_ARM64_SYSROOT  aarch64 glibc sysroot (required)
#         LLVM_SRC           llvm-project at llvmorg-14.0.6, the prebuilt clang's revision (required)
set -eu
AOSP="${ACL_AOSP_ROOT:-/mnt/dev/aosp}"
OUT="${1:-$AOSP/../glibc-arm64-runtimes}"
: "${ACL_ARM64_SYSROOT:?set ACL_ARM64_SYSROOT to an aarch64 glibc sysroot}"
: "${LLVM_SRC:?set LLVM_SRC to an llvm-project checkout at llvmorg-14.0.6}"
SYSROOT="$ACL_ARM64_SYSROOT"
CLANG="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d"
NINJA="$AOSP/prebuilts/build-tools/linux-x86/bin/ninja"
TRIPLE=aarch64-linux-gnu
WORK="$OUT/work"; mkdir -p "$WORK"

# The prebuilt's LLVMExports.cmake names LLVM libraries the prebuilt does not ship. compiler-rt does
# not need them, so the file is emptied for the configure step and restored on exit.
CMAKEDIR="$CLANG/lib64/cmake/llvm"
cp -n "$CMAKEDIR/LLVMExports.cmake" "$WORK/LLVMExports.cmake.bak"
trap 'cp "$WORK/LLVMExports.cmake.bak" "$CMAKEDIR/LLVMExports.cmake"' EXIT
: > "$CMAKEDIR/LLVMExports.cmake"

F="--sysroot=$SYSROOT --gcc-toolchain=$SYSROOT/usr -fPIC"
B="$WORK/build"; rm -rf "$B"
# CMAKE_POLICY_VERSION_MINIMUM: cmake 4 refuses LLVM 14's declared minimum without it.
cmake -G Ninja -S "$LLVM_SRC/compiler-rt" -B "$B" \
  -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CLANG/bin/clang" -DCMAKE_CXX_COMPILER="$CLANG/bin/clang++" \
  -DCMAKE_ASM_COMPILER="$CLANG/bin/clang" \
  -DCMAKE_AR="$CLANG/bin/llvm-ar" -DCMAKE_RANLIB="$CLANG/bin/llvm-ranlib" \
  -DCMAKE_C_COMPILER_TARGET=$TRIPLE -DCMAKE_CXX_COMPILER_TARGET=$TRIPLE \
  -DCMAKE_ASM_COMPILER_TARGET=$TRIPLE \
  -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DCMAKE_ASM_FLAGS="$F" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DLLVM_CONFIG_PATH="$CLANG/bin/llvm-config" \
  -DCOMPILER_RT_STANDALONE_BUILD=ON -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=OFF -DCOMPILER_RT_BUILD_PROFILE=ON \
  -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF \
  -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BUILD_ORC=OFF \
  -DCOMPILER_RT_INCLUDE_TESTS=OFF -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF > "$WORK/cmake.log" 2>&1
"$NINJA" -C "$B" builtins profile > "$WORK/ninja.log" 2>&1
find "$B" -name 'libclang_rt.builtins-aarch64.a' -exec cp {} "$OUT/" \;
find "$B" -name 'libclang_rt.profile-aarch64.a' -exec cp {} "$OUT/" \;

# The minimal UBSan runtime is one freestanding source file.
"$CLANG/bin/clang++" --target=$TRIPLE $F -O2 -fno-rtti -fno-exceptions -fno-sanitize=all \
  -fvisibility=hidden -nostdinc++ -I"$LLVM_SRC/compiler-rt/lib" \
  -c "$LLVM_SRC/compiler-rt/lib/ubsan_minimal/ubsan_minimal_handlers.cpp" -o "$WORK/ubsan_minimal.o"
"$CLANG/bin/llvm-ar" rcs "$OUT/libclang_rt.ubsan_minimal-aarch64.a" "$WORK/ubsan_minimal.o"
ls -l "$OUT"/*.a

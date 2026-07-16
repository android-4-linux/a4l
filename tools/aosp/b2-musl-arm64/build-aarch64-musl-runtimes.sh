#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl build tooling. Builds LLVM runtimes (Apache-2.0 w/ LLVM exception) and a
# musl sysroot (MIT) with AOSP's prebuilt clang. Consumes AOSP prebuilts + external/musl (Apache-2.0
# / MIT). No GPL-3.0 sources.
#
# B2 step 2 of 2: produce the aarch64-linux-musl HOST LLVM runtime set that the clang prebuilt ships
# for x86_64-linux-musl but not for arm64 (see ./README.md). The B2 Soong toolchain (step 1,
# soong-musl-arm64.patch) is proven; this fills the one remaining dependency so ART can link.
#
# Inputs (edit to taste):
AOSP="${ACL_AOSP_ROOT:-/mnt/dev/aosp}"
CLANG="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d"   # 14.0.6 / r450784d
LLVM_SRC="${LLVM_SRC:-/mnt/dev/llvm-project-src}"             # git clone --branch llvmorg-14.0.6 (matches the prebuilt)
NINJA="$AOSP/prebuilts/build-tools/linux-x86/bin/ninja"
CMAKE="${CMAKE:-/usr/bin/cmake}"
TRIPLE=aarch64-linux-musl
WORK="${WORK:-/mnt/dev/b2-runtimes}"
SYSROOT="$WORK/musl-sysroot"
STAGE="$WORK/stage/lib64/clang/14.0.6/lib/linux"             # mirrors the clang prebuilt layout
set -eu
mkdir -p "$WORK" "$STAGE"

# cmake 4.x vs LLVM-14 cmake files: allow the old minimum. The prebuilt clang's LLVMExports.cmake
# references LLVM libs absent from the prebuilt; it is included behind `if(NOT TARGET LLVMSupport)`
# and compiler-rt/libcxx do not need it, so we stub it for the build and restore it after.
CMAKEDIR="$CLANG/lib64/cmake/llvm"
stub_exports()   { cp -n "$CMAKEDIR/LLVMExports.cmake" "$WORK/LLVMExports.cmake.bak" 2>/dev/null || true; : > "$CMAKEDIR/LLVMExports.cmake"; }
unstub_exports() { [ -f "$WORK/LLVMExports.cmake.bak" ] && cp "$WORK/LLVMExports.cmake.bak" "$CMAKEDIR/LLVMExports.cmake"; }
trap unstub_exports EXIT

CC="$CLANG/bin/clang"; CXX="$CLANG/bin/clang++"
# -nostdlibinc keeps clang's freestanding headers but not the host's; -isystem points at musl.
MINC="-nostdlibinc -isystem $SYSROOT/include"

echo "== 1. musl-$TRIPLE sysroot (musl headers + kernel UAPI headers) =="
if [ ! -f "$SYSROOT/include/bits/alltypes.h" ]; then
  B="$WORK/musl-build"; rm -rf "$B"; mkdir -p "$B"; cd "$B"
  "$AOSP/external/musl/configure" --target="$TRIPLE" --prefix="$SYSROOT" \
    CC="$CC" AR="$CLANG/bin/llvm-ar" RANLIB="$CLANG/bin/llvm-ranlib" CFLAGS="--target=$TRIPLE"
  # install-headers gives the musl header tree incl. the arch-generated bits/alltypes.h. (The full
  # `make` also builds libc.a; the libc.so link needs extra target crt setup and is not needed here —
  # the runtimes are static, and Soong links against libc_musl at ART link time.)
  make -j"$(nproc)" install-headers
fi
# libc++ (atomic.cpp) needs Linux kernel UAPI headers. Use AOSP's sanitized set (Apache-2.0/BSD, not
# the GPL-2.0 originals): the main uapi tree + the android/uapi overlay (which carries linux/compiler.h).
if [ ! -f "$SYSROOT/include/linux/futex.h" ]; then
  U="$AOSP/bionic/libc/kernel/uapi"; AU="$AOSP/bionic/libc/kernel/android/uapi"
  cp -r "$U/linux" "$U/asm-generic" "$SYSROOT/include/"
  cp -r "$U/asm-arm64/asm" "$SYSROOT/include/"
  cp -r "$AU/linux/." "$SYSROOT/include/linux/"    # linux/compiler.h etc.
fi

echo "== 2. compiler-rt (crt + builtins) for $TRIPLE =="
B="$WORK/compiler-rt-build"; rm -rf "$B"
stub_exports
"$CMAKE" -G Ninja -S "$LLVM_SRC/compiler-rt" -B "$B" \
  -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_ASM_COMPILER="$CC" \
  -DCMAKE_C_COMPILER_TARGET="$TRIPLE" -DCMAKE_ASM_COMPILER_TARGET="$TRIPLE" \
  -DCMAKE_C_FLAGS="$MINC" -DCMAKE_ASM_FLAGS="$MINC" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DLLVM_CONFIG_PATH="$CLANG/bin/llvm-config" \
  -DCOMPILER_RT_STANDALONE_BUILD=ON -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=ON -DCOMPILER_RT_BUILD_PROFILE=ON \
  -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
  -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BUILD_ORC=OFF \
  -DCOMPILER_RT_INCLUDE_TESTS=OFF -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF
"$NINJA" -C "$B" builtins crt profile   # profile: ART's host build references libclang_rt.profile
find "$B" -name 'clang_rt.crt*-aarch64.o' -exec cp {} "$STAGE/" \;
find "$B" -name 'libclang_rt.builtins-aarch64.a' -o -name 'libclang_rt.profile-aarch64.a' -exec cp {} "$STAGE/" \;
unstub_exports

echo "== 3. libunwind + libc++abi + libc++ for $TRIPLE (static) =="
B="$WORK/cxx-build"; rm -rf "$B"
stub_exports
"$CMAKE" -G Ninja -S "$LLVM_SRC/runtimes" -B "$B" \
  -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_COMPILER_TARGET="$TRIPLE" -DCMAKE_CXX_COMPILER_TARGET="$TRIPLE" \
  -DCMAKE_C_FLAGS="$MINC" -DCMAKE_CXX_FLAGS="$MINC" \
  -DCMAKE_SYSROOT="$SYSROOT" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
  -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_SHARED=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXX_HAS_MUSL_LIBC=ON \
  -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF
"$NINJA" -C "$B"
find "$B" -name 'libc++*.a' -o -name 'libunwind*.a' | while read -r f; do cp "$f" "$STAGE/../"; done
unstub_exports

echo ""
echo "DONE. aarch64-linux-musl runtime set staged under: $WORK/stage"
echo "Next: drop these under \$CLANG/lib64/clang/14.0.6/lib/linux/aarch64-linux-musl/ (mirror the"
echo "x86_64-linux-musl layout), then apply soong-musl-arm64.patch and build ART for linux_musl_arm64."

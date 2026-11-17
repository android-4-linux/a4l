#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l build tooling. Runs Skia's own GN build (BSD-3-Clause) with the
# arguments of wayland-cxx-scanner's scripts/build-skia.sh (MIT) plus a cross toolchain.
# No GPL-3.0 sources.
#
# Cross-build the pinned Skia for arm64 glibc and install the prefix a4l's Meson build takes as
# -Dskia_prefix. The x86-64 prefix comes from wayland-cxx-scanner's build-skia.sh; this is the
# same GN configuration with a clang cross toolchain, the sysroot's libc++ (the one a4l links),
# bundled FreeType and no fontconfig or X11, which a target sysroot often lacks.
#
# Usage:  build-skia.sh PREFIX
# Env:    SKIA_SRC            Skia checkout at the sha in third_party/skia/METADATA, after
#                             tools/git-sync-deps, bin/fetch-gn and bin/fetch-ninja (required)
#         ACL_ARM64_SYSROOT   arm64 sysroot (required)
#         ACL_ARM64_LIBCXX    libc++ header directory (default: the sysroot's llvm-18)
set -eu
PREFIX="${1:?usage: build-skia.sh PREFIX}"
SRC="${SKIA_SRC:?set SKIA_SRC to a Skia checkout}"
SR="${ACL_ARM64_SYSROOT:?set ACL_ARM64_SYSROOT}"
CXXINC="${ACL_ARM64_LIBCXX:-$SR/usr/lib/llvm-18/include/c++/v1}"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PIN=$(sed -n 's/^sha: "\([0-9a-f]\{40\}\)"$/\1/p' "$HERE/../../../third_party/skia/METADATA")
HEAD=$(git -C "$SRC" rev-parse HEAD)
[ -n "$PIN" ] && [ "$HEAD" = "$PIN" ] || { echo "Skia at $HEAD, pin is $PIN" >&2; exit 1; }
[ -d "$CXXINC" ] || { echo "no libc++ headers at $CXXINC" >&2; exit 1; }
[ -x "$SRC/bin/gn" ] && [ -x "$SRC/third_party/ninja/ninja" ] ||
  { echo "run bin/fetch-gn and bin/fetch-ninja in $SRC" >&2; exit 1; }

T="\"--target=aarch64-linux-gnu\",\"--sysroot=$SR\",\"--gcc-toolchain=$SR/usr\""
OUT="$SRC/out/arm64-glibc"
"$SRC/bin/gn" gen "$OUT" --root="$SRC" --args="target_cpu=\"arm64\" \
cc=\"clang\" cxx=\"clang++\" ar=\"llvm-ar\" \
is_official_build=true is_component_build=false is_debug=false \
skia_use_system_harfbuzz=false skia_use_system_icu=false \
skia_use_system_libpng=false skia_use_system_libjpeg_turbo=false \
skia_use_system_libwebp=false skia_use_system_zlib=false \
skia_use_system_expat=false skia_use_system_freetype2=false \
skia_use_fontconfig=false skia_use_x11=false \
skia_use_gl=true skia_use_vulkan=true skia_use_egl=true \
skia_use_icu=true skia_use_harfbuzz=true \
skia_enable_skparagraph=true \
skia_enable_svg=false skia_enable_pdf=false skia_enable_skottie=false \
extra_cflags=[$T,\"-fPIC\"] extra_asmflags=[$T] \
extra_cflags_cc=[\"-nostdinc++\",\"-isystem\",\"$CXXINC\",\"-D_LIBCPP_DISABLE_AVAILABILITY\"] \
extra_ldflags=[$T,\"-fuse-ld=lld\"]"
"$SRC/third_party/ninja/ninja" -C "$OUT" skia skparagraph skshaper skunicode_core skunicode_icu

# The layout wayland-cxx-scanner's script installs: headers keep the paths Skia's own
# includes use, and src/ ships headers only.
mkdir -p "$PREFIX/lib" "$PREFIX/modules/skcms/src"
cp -a "$SRC/include" "$PREFIX/"
( cd "$SRC" && find src -name '*.h' -print0 | cpio -0 -pdm --quiet "$PREFIX" )
for m in skparagraph skshaper skunicode; do
  mkdir -p "$PREFIX/modules/$m"
  cp -a "$SRC/modules/$m/include" "$PREFIX/modules/$m/"
  [ -d "$SRC/modules/$m/src" ] &&
    ( cd "$SRC" && find "modules/$m/src" -name '*.h' -print0 | cpio -0 -pdm --quiet "$PREFIX" )
done
cp -a "$SRC/modules/skcms/skcms.h" "$PREFIX/modules/skcms/"
cp -a "$SRC/modules/skcms/src/"*.h "$PREFIX/modules/skcms/src/"
cp -a "$OUT"/*.a "$PREFIX/lib/"
echo "installed $(find "$PREFIX/lib" -name '*.a' | wc -l) archives at $PREFIX ($PIN)"

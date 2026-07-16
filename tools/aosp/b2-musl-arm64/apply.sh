#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl build tooling. Patches AOSP Soong/make/prebuilts + a handful of module
# Android.bp files (all Apache-2.0) to register a linux_musl_arm64 host toolchain, map the
# aarch64-linux-musl LLVM runtimes into module variants, and disable the host subsystems/modules that
# don't support the new host (fuzzers, sanitizers, LLVM/RenderScript, Rust, Python, and non-ART test
# tools). No GPL-3.0 sources. See ./README.md.
#
# Applies soong-musl-arm64.patch per-repo (the patch groups hunks under "### repo: <path> ###" lines).
# Usage:   ./apply.sh [AOSP_ROOT]         apply
#          ./apply.sh --revert [AOSP_ROOT] revert every touched file
# Also (re)integrates the built runtime .a/.o into the clang prebuilt from the staged tree
# (default /mnt/dev/aarch64-musl-runtimes; override with ACL_B2_RUNTIMES).
set -eu
REVERT=""; [ "${1:-}" = "--revert" ] && { REVERT=1; shift; }
AOSP="${1:-${ACL_AOSP_ROOT:-/mnt/dev/aosp}}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PATCH="$HERE/soong-musl-arm64.patch"
RUNTIMES="${ACL_B2_RUNTIMES:-/mnt/dev/aarch64-musl-runtimes}"
CLANGRES="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d/lib64/clang/14.0.6/lib/linux"

# The repos the patch touches (must match the "### repo:" headers in soong-musl-arm64.patch).
REPOS="build/soong build/make prebuilts/clang/host/linux-x86 external/llvm packages/modules/Bluetooth
       system/extras/simpleperf system/tools/mkbootimg test/vts system/extras/libfec"

if [ -n "$REVERT" ]; then
  for r in $REPOS; do git -C "$AOSP/$r" checkout -- . 2>/dev/null || true; git -C "$AOSP/$r" clean -fdq -- . 2>/dev/null || true; done
  rm -rf "$CLANGRES/aarch64-linux-musl" "$CLANGRES"/clang_rt.crt*-aarch64.o "$CLANGRES"/libclang_rt.builtins-aarch64.a
  echo "reverted all B2 changes under $AOSP"
  exit 0
fi

# Split the patch by "### repo:" markers and git-apply each section in its repo.
awk -v out="/tmp/b2patch" '
  /^### repo: / { repo=$3; f=out"."NR; print repo > (out".map."NR); mapfile[++n]=NR; next }
  { if (f) print > f }
  END {}' "$PATCH" >/dev/null 2>&1 || true
# Simpler robust split in bash:
tmp=$(mktemp -d); cur="";
while IFS= read -r line; do
  case "$line" in
    "### repo: "*)
      cur="${line#### repo: }"; cur="${cur% ###}"; cur="${cur% (NEW FILE)}"
      safe=$(printf '%s' "$cur" | tr '/' '_'); curf="$tmp/$safe.patch"; : > "$curf" ;;
    "#"*) : ;;  # skip leading comments
    *) [ -n "${curf:-}" ] && printf '%s\n' "$line" >> "$curf" ;;
  esac
done < "$PATCH"
for pf in "$tmp"/*.patch; do
  [ -s "$pf" ] || continue
  repo=$(basename "$pf" .patch | tr '_' '/')
  git -C "$AOSP/$repo" apply --whitespace=nowarn "$pf" 2>/dev/null \
    && echo "  applied: $repo" \
    || echo "  SKIP/партial (already applied or offset): $repo"
done
rm -rf "$tmp"

# (Re)integrate the built runtimes into the clang prebuilt.
if [ -d "$RUNTIMES" ]; then
  ST="$RUNTIMES/lib64/clang/14.0.6/lib/linux"
  mkdir -p "$CLANGRES/aarch64-linux-musl/lib/linux"
  cp "$ST"/clang_rt.crt*-aarch64.o "$ST"/libclang_rt.builtins-aarch64.a "$CLANGRES/aarch64-linux-musl/lib/linux/" 2>/dev/null || true
  find "$RUNTIMES" -name 'libclang_rt.profile-aarch64.a' -exec cp {} "$CLANGRES/aarch64-linux-musl/lib/linux/" \; 2>/dev/null || true
  cp "$ST"/aarch64-linux-musl/lib/*.a "$CLANGRES/aarch64-linux-musl/lib/" 2>/dev/null || true
  cp "$ST"/clang_rt.crt*-aarch64.o "$ST"/libclang_rt.builtins-aarch64.a "$CLANGRES/" 2>/dev/null || true
  echo "  integrated runtimes from $RUNTIMES"
else
  echo "  NOTE: staged runtimes not at $RUNTIMES — rebuild with build-aarch64-musl-runtimes.sh"
fi
echo "done. Build with: HOST_CROSS_OS=linux_musl HOST_CROSS_ARCH=arm64 m --skip-soong-tests <target>"

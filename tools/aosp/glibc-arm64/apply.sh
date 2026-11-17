#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l build tooling. Patches AOSP Soong, make, ART, libcutils and the clang
# prebuilt's Soong glue (all Apache-2.0) to add a linux_glibc x arm64 host target. No GPL-3.0
# sources. See ./README.md.
#
# Usage:  apply.sh [--revert] [AOSP_ROOT]
# Env:    ACL_ARM64_SYSROOT   an aarch64 glibc sysroot in Debian multiarch layout (required to apply)
#         ACL_ARM64_RUNTIMES  output of build-runtimes.sh (default: $AOSP_ROOT/../glibc-arm64-runtimes)
set -eu
REVERT=""; [ "${1:-}" = "--revert" ] && { REVERT=1; shift; }
AOSP="${1:-${ACL_AOSP_ROOT:-/mnt/dev/aosp}}"
HERE="$(cd "$(dirname "$0")" && pwd)"
RUNTIMES="${ACL_ARM64_RUNTIMES:-$AOSP/../glibc-arm64-runtimes}"
RES="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d/lib64/clang/14.0.6/lib/linux"
HOSTGCC="$AOSP/prebuilts/gcc/linux-x86/host"
GOFILE="$AOSP/build/soong/cc/config/arm64_linux_glibc_host.go"
RT="libclang_rt.builtins-aarch64.a libclang_rt.profile-aarch64.a libclang_rt.ubsan_minimal-aarch64.a"

# patch file name -> repository path
repo_of() {
  case "$1" in
    build-soong) echo build/soong ;;
    build-make) echo build/make ;;
    art) echo art ;;
    system-core) echo system/core ;;
    prebuilts-clang-host-linux-x86) echo prebuilts/clang/host/linux-x86 ;;
    *) echo "unknown patch $1" >&2; exit 1 ;;
  esac
}

rc=0
for pf in "$HERE"/patches/*.patch; do
  repo=$(repo_of "$(basename "$pf" .patch)")
  if [ -n "$REVERT" ]; then
    if git -C "$AOSP/$repo" apply -R --check "$pf" 2>/dev/null; then
      git -C "$AOSP/$repo" apply -R "$pf"; echo "  reverted: $repo"
    else
      echo "  not applied, left alone: $repo"
    fi
  elif git -C "$AOSP/$repo" apply -R --check "$pf" 2>/dev/null; then
    # Checked first: a patch that only inserts lines can apply a second time.
    echo "  already applied: $repo"
  elif git -C "$AOSP/$repo" apply --check "$pf" 2>/dev/null; then
    git -C "$AOSP/$repo" apply "$pf"; echo "  applied: $repo"
  else
    echo "  FAILED to apply: $repo ($pf)" >&2; rc=1
  fi
done

if [ -n "$REVERT" ]; then
  rm -f "$GOFILE" "$HOSTGCC/aarch64-linux-glibc2.31"
  rm -f "$HOSTGCC/aarch64-linux-glibc2.31-overlay/include/sys/capability.h"
  rmdir -p "$HOSTGCC/aarch64-linux-glibc2.31-overlay/include/sys" 2>/dev/null || true
  for f in $RT; do rm -f "$RES/$f"; done
  echo "reverted the glibc arm64 host target under $AOSP"
  exit $rc
fi

: "${ACL_ARM64_SYSROOT:?set ACL_ARM64_SYSROOT to an aarch64 glibc sysroot}"
[ -f "$ACL_ARM64_SYSROOT/usr/include/aarch64-linux-gnu/bits/local_lim.h" ] ||
  { echo "$ACL_ARM64_SYSROOT is not an aarch64 Debian-layout sysroot" >&2; exit 1; }
cp "$HERE/arm64_linux_glibc_host.go" "$GOFILE"
ln -sfn "$ACL_ARM64_SYSROOT" "$HOSTGCC/aarch64-linux-glibc2.31"
mkdir -p "$HOSTGCC/aarch64-linux-glibc2.31-overlay/include/sys"
cp "$HERE/overlay/include/sys/capability.h" "$HOSTGCC/aarch64-linux-glibc2.31-overlay/include/sys/"
for f in $RT; do
  if [ -f "$RUNTIMES/$f" ]; then cp "$RUNTIMES/$f" "$RES/"; else echo "  MISSING runtime $f (run build-runtimes.sh)" >&2; rc=1; fi
done
echo "done. Build with:"
echo "  HOST_CROSS_OS=linux_glibc HOST_CROSS_ARCH=arm64 ALLOW_MISSING_DEPENDENCIES=true \\"
echo "    m --skip-soong-tests --soong-only <out/soong/.intermediates/.../linux_glibc_arm64_shared/...>"
exit $rc

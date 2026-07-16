#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original bring-up helper; docs/pi-plan.md (P-B/M2), docs/art-host-spike.md (the
# ACL_ART_HOME layout). Consumes AOSP soong intermediates (Apache-2.0). No GPL-3.0 sources.
#
# Assemble a linux_bionic-arm64 ACL_ART_HOME tree from an AOSP tree that has already built the
# host-cross ART artifacts (see docs/pi-plan.md P-B: HOST_CROSS_OS=linux_bionic HOST_CROSS_ARCH=arm64
# m <the linux_bionic_arm64 outputs>). Produces a tree you scp to the Pi and point ACL_ART_HOME at.
#
# Usage:  tools/assemble-arm64-art.sh [DEST_DIR] [AOSP_ROOT]
#   DEST_DIR   default /tmp/acl-art-arm64
#   AOSP_ROOT  default $ACL_AOSP_ROOT or /mnt/dev/aosp
# Env: ACL_ART_STRIP=1 strips the libs (much smaller; loses crash symbols).
set -u
R="${2:-${ACL_AOSP_ROOT:-/mnt/dev/aosp}}"
INT="$R/out/soong/.intermediates"
DEST="${1:-/tmp/acl-art-arm64}"
STRIP="${ACL_ART_STRIP:-}"; STRIP_TOOL="${CROSS_STRIP:-aarch64-linux-gnu-strip}"
rm -rf "$DEST"; mkdir -p "$DEST/lib64" "$DEST/bin" "$DEST/apex/com.android.art/javalib"

IDX=$(mktemp)
find "$INT" -path '*linux_bionic_arm64_shared/*.so' -type f 2>/dev/null > "$IDX"
echo "indexed $(wc -l < "$IDX") linux_bionic_arm64 shared libs"

declare -A seen
resolve() {  # transitive NEEDED closure into lib64/
  local so="$1" base; base=$(basename "$so")
  [ -n "${seen[$base]:-}" ] && return; seen[$base]=1
  cp -u "$so" "$DEST/lib64/" 2>/dev/null
  local dep p
  for dep in $(readelf -d "$so" 2>/dev/null | awk '/NEEDED/{gsub(/[][]/,"",$5);print $5}'); do
    [ -n "${seen[$dep]:-}" ] && continue
    p=$(grep -m1 "/${dep}\$" "$IDX")
    [ -n "$p" ] && resolve "$p" || echo "  UNRESOLVED: $dep"
  done
}
resolve "$INT/art/runtime/libart/linux_bionic_arm64_shared/libart.so"
resolve "$INT/frameworks/base/core/jni/libandroid_runtime/linux_bionic_arm64_shared/libandroid_runtime.so"
# ART loads these core native libs at runtime via System.loadLibrary in Runtime::InitNativeMethods()
# — NOT part of libart's NEEDED closure, so seed them explicitly (their own NEEDED is resolved too).
# Build first: HOST_CROSS_OS=linux_bionic HOST_CROSS_ARCH=arm64 m <their linux_bionic_arm64 outputs>.
for nl in libicu_jni libjavacore libopenjdk libandroidio libnativehelper; do
  p=$(grep -m1 "/${nl}.so\$" "$IDX"); [ -n "$p" ] && resolve "$p" || echo "  NOTE: $nl.so not built (m it for linux_bionic_arm64)"
done
cp -u "$INT/bionic/linker/ld-android/linux_bionic_arm64_shared/ld-android.so" "$DEST/lib64/" 2>/dev/null
cp -u "$INT/art/dex2oat/dex2oats/linux_bionic_arm64/dex2oats" "$DEST/bin/" 2>/dev/null

# Boot jars are DEX (arch-independent) — reuse the already-built x86 host apex jars. ArtVm/art-host
# expect them ALL under apex/<apex-name>/javalib/ (matching kBcp), so the i18n/conscrypt jars go under
# apex/ too (not a bare com.android.*/javalib/ — that path is not on the boot classpath).
X="$R/out/host/linux-x86"
for j in core-oj core-libart okhttp bouncycastle apache-xml; do
  cp -u "$X/apex/com.android.art/javalib/$j.jar" "$DEST/apex/com.android.art/javalib/" 2>/dev/null
done
mkdir -p "$DEST/apex/com.android.i18n/javalib" "$DEST/apex/com.android.conscrypt/javalib"
cp -u "$X/apex/com.android.i18n/javalib/core-icu4j.jar" "$DEST/apex/com.android.i18n/javalib/" 2>/dev/null
cp -u "$X/apex/com.android.conscrypt/javalib/conscrypt.jar" "$DEST/apex/com.android.conscrypt/javalib/" 2>/dev/null
# ICU + tzdata *data* roots (arch-independent) — acl sets ANDROID_I18N_ROOT / ANDROID_TZDATA_ROOT.
for d in com.android.i18n/etc/icu com.android.tzdata/etc/tz com.android.tzdata/etc/tzdata; do
  [ -e "$X/$d" ] && { mkdir -p "$DEST/$(dirname "$d")"; cp -ru "$X/$d" "$DEST/$d" 2>/dev/null; }
done

if [ -n "$STRIP" ]; then
  command -v "$STRIP_TOOL" >/dev/null 2>&1 && for f in "$DEST"/lib64/*.so "$DEST"/bin/*; do "$STRIP_TOOL" "$f" 2>/dev/null; done
fi
rm -f "$IDX"
echo "ACL_ART_HOME tree at $DEST:"
echo "  lib64: $(ls "$DEST/lib64" | wc -l) libs ($(du -sh "$DEST/lib64" 2>/dev/null | cut -f1))"
echo "  bin:   $(ls "$DEST/bin" 2>/dev/null | wc -l);  boot jars: $(find "$DEST" -name '*.jar' | wc -l)"
echo "Next (on the Pi): export ACL_ART_HOME=<copied tree>; see docs/pi-plan.md M2/M3."

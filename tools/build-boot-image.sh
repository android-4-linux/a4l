#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl build tooling; AOSP host dex2oat (Apache-2.0) invoked as a
# black box per its documented --image/--dex-file/--dex-location interface. No GPL-3.0 sources.
#
# Build an ART boot image for EXACTLY the bootclasspath acl runs with, so the framework
# executes AOT-compiled instead of interpreted.
#
# WHY: without a boot image acl passes -Xint and ART interprets every method (roughly an
# order of magnitude slower; a heavy app looks unresponsive). The prebuilt AOSP host image
# cannot be reused because acl appends aclshims.jar to the bootclasspath, and ART rejects
# an image whose baked components do not match ("Image component ... does not match a boot
# class path component") and falls back to imageless.
#
# The image bakes in the dex LOCATIONS, so every jar must sit at the same path here and at
# run time. run-apk.sh therefore keeps aclshims.jar at a stable path
# (build/framework/artjars/) and picks this image up automatically once it exists.
#
# KNOWN BLOCKER (why run-apk.sh does not use this by default): with the framework AOT-compiled,
# acl's replacement android.graphics natives crash --
#   SIGSEGV in Typeface_nativeGetStyle -> SkTypeface::fontStyle(), called from
#   boot-framework-minus-apex.oat android.graphics.Typeface.<init>
# The image pre-initializes boot classes and bakes their static state, including native pointers
# minted inside dex2oat, which are meaningless at run time; acl's natives reinterpret_cast the
# stored long without being able to validate it. Preloading nothing (--image-classes with an empty
# list) is rejected by dex2oat, so the fix needs a curated image-classes list that excludes
# android.graphics, or acl-side revalidation of the native handles. Until then set
# ACL_USE_BOOT_IMAGE=1 to opt in for experiments.
#
# Usage:   tools/build-boot-image.sh [output-dir]      (default: build/framework/artjars)
# Env:     ACL_ART_HOME  (required) host ART build out/host/linux-x86
# Exits 77 (meson "skip") when the host ART build or the jars are absent.
set -u
unset -f grep egrep fgrep sed awk ls find 2>/dev/null || true

SKIP=77
die_skip() { printf 'SKIP: %s\n' "$1" >&2; exit "$SKIP"; }

HOME_DIR="${ACL_ART_HOME:-}"
[ -n "$HOME_DIR" ] && [ -d "$HOME_DIR" ] || die_skip "ACL_ART_HOME not set to a host ART build"
ROOT=$(cd "$HOME_DIR/../../.." && pwd)
ACLROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT="${1:-$ACLROOT/build/framework/artjars}"
mkdir -p "$OUT"

DEX2OAT=$(command -v "$HOME_DIR/bin/dex2oat64" || command -v "$HOME_DIR/bin/dex2oat")
[ -x "${DEX2OAT:-}" ] || die_skip "dex2oat not found under $HOME_DIR/bin"

ACLSHIMS="$OUT/aclshims.jar"
[ -f "$ACLSHIMS" ] || die_skip "no $ACLSHIMS — run tools/run-apk.sh once first to build the shim jars"

first() { for p in "$@"; do [ -e "$p" ] && { printf '%s\n' "$p"; return; }; done; }

# The bootclasspath, in the SAME order jvm_vm.cpp assembles it: the apex core jars first,
# then the framework jars, then acl's shims. Order is part of the image identity.
BCP=()
for spec in \
  com.android.art:core-oj com.android.art:core-libart com.android.art:okhttp \
  com.android.art:bouncycastle com.android.art:apache-xml \
  com.android.i18n:core-icu4j com.android.conscrypt:conscrypt; do
  apex="${spec%%:*}"; mod="${spec##*:}"
  j="$HOME_DIR/apex/$apex/javalib/$mod.jar"
  [ -f "$j" ] || die_skip "missing apex boot jar $j"
  BCP+=("$j")
done
# Same unique-name symlinks run-apk.sh puts on the bootclasspath (see there): all six framework
# modules build to a file called javalib.jar, which would collide as boot-javalib.art components.
BCPLINKS="$OUT/bcp"
mkdir -p "$BCPLINKS"
for m in framework-minus-apex framework-graphics ext telephony-common voip-common ims-common; do
  j=$(first "$ROOT"/out/target/product/*/obj/JAVA_LIBRARIES/${m}_intermediates/javalib.jar)
  if [ -n "$j" ]; then
    ln -sfn "$j" "$BCPLINKS/${m}.jar"
    BCP+=("$BCPLINKS/${m}.jar")
  fi
done
BCP+=("$ACLSHIMS")

# dex2oat wants each dex file and its location; acl runs from these same absolute paths, so
# location == file for every entry.
ARGS=()
for j in "${BCP[@]}"; do ARGS+=(--dex-file="$j" --dex-location="$j"); done
BCP_JOINED=$(IFS=:; printf '%s' "${BCP[*]}")

# ART resolves -Ximage:<dir>/boot.art as <dir>/<instruction-set>/boot.art, so the image must be
# emitted into the ISA subdirectory (this mirrors AOSP's own framework/x86_64/ layout) while
# ACL_ART_BOOT_IMAGE keeps pointing at the un-suffixed location.
ISA=x86_64
IMGDIR="$OUT/$ISA"
mkdir -p "$IMGDIR"
printf 'building boot image for %d bootclasspath jars\n  -> %s\n' "${#BCP[@]}" "$IMGDIR/boot.art" >&2
rm -f "$IMGDIR"/boot*.art "$IMGDIR"/boot*.oat "$IMGDIR"/boot*.vdex 2>/dev/null

# The boot image is multi-image: dex2oat rejects --single-image for a primary boot image on host,
# and names each component after its jar basename. All six framework modules build to a file called
# javalib.jar, so they would collide on boot-javalib.art/.oat and overwrite each other (ART then
# rejects the image: "expected 1 uncompressed dex files, but found 4 in .../ims-common/javalib.jar").
# The unique-name symlinks above are what makes multi-image naming work.
# --compiler-filter=speed compiles without needing a boot profile ("Full boot image cannot be
# compiled because no profile is provided").
set -x
"$DEX2OAT" \
  --instruction-set=x86_64 \
  --instruction-set-features=default \
  --compiler-filter=speed \
  --image="$IMGDIR/boot.art" \
  --oat-file="$IMGDIR/boot.oat" \
  --base=0x70000000 \
  --runtime-arg -Xms64m --runtime-arg -Xmx512m \
  --runtime-arg -Xbootclasspath:"$BCP_JOINED" \
  --runtime-arg -Xbootclasspath-locations:"$BCP_JOINED" \
  --android-root="$HOME_DIR" \
  --host \
  "${ARGS[@]}" 2>&1 | tail -25
rc=${PIPESTATUS[0]}
set +x

if [ "$rc" -ne 0 ] || [ ! -f "$IMGDIR/boot.art" ]; then
  printf 'dex2oat failed (rc=%s); acl will keep running interpreted (-Xint)\n' "$rc" >&2
  exit 1
fi
printf 'boot image built:\n' >&2
ls -la "$IMGDIR"/boot.art "$IMGDIR"/boot.oat 2>/dev/null >&2
printf '\nrun-apk.sh picks this up automatically (ACL_ART_BOOT_IMAGE=%s)\n' "$OUT/boot.art" >&2

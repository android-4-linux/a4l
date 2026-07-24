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
# FIXED: the Typeface_nativeGetStyle SIGSEGV that used to block the full image was not the image
# baking in stale pointers -- acl declared the font natives with the normal JNI signature while the
# framework annotates them @CriticalNative, so every argument was read from the wrong register. With
# the signatures corrected (and handles validated) in graphics_jni.cpp, the FULL image runs clean:
# octalsuntime 1320 present frames in 45s vs 180 interpreted, ~7.3x.
#
# Still opt-in (ACL_USE_BOOT_IMAGE=1) for a different, unexplained reason: WebView apps that run
# fine imageless produce no frames under the full image (openmaps: 420 frames imageless, 0 with the
# image, no crash and no exception logged). Non-WebView apps are unaffected.
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
# ART accepts a boot image that covers a PREFIX of the bootclasspath; the rest is loaded from dex
# as usual. The apex core jars above are exactly that prefix, and they hold the java.* code every
# app hammers. The image covers CORE ONLY by default (~1.6x), which is the safe choice for every
# app including WebView ones.
#
# ACL_BOOT_IMAGE_FULL=1 includes the framework jars too and is much faster (~7.3x) now that the
# font-native ABI is fixed -- use it for non-WebView apps; see the header for the WebView caveat.
# It also needs a smaller heap than core-only does (ACL_ART_HEAP_MB=128; ART reserves 2x -Xmx for
# region space and the larger image leaves less room).
# The unique-name symlinks exist because all six framework modules build to a file called
# javalib.jar and would otherwise collide as boot-javalib.art components.
if [ -n "${ACL_BOOT_IMAGE_FULL:-}" ]; then
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
fi

# dex2oat wants each dex file and its location; acl runs from these same absolute paths, so
# location == file for every entry.
ARGS=()
for j in "${BCP[@]}"; do ARGS+=(--dex-file="$j" --dex-location="$j"); done
BCP_JOINED=$(IFS=:; printf '%s' "${BCP[*]}")

# ART resolves -Ximage:<dir>/boot.art as <dir>/<instruction-set>/boot.art, so the image must be
# emitted into the ISA subdirectory (this mirrors AOSP's own framework/x86_64/ layout) while
# ACL_ART_BOOT_IMAGE keeps pointing at the un-suffixed location.
# The image must not sit where ART wants its (512MB) region-space heap, or startup aborts with
# "Failed to allocate pages for alloc space (main space (region space))" /
# "Check failed: region_space_mem_map.IsValid()". 0x70000000 (the usual target base) collides on
# this host; keep the image low and leave the upper range free for the heap.
IMAGE_BASE="${ACL_IMAGE_BASE:-0x20000000}"
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
  --base="$IMAGE_BASE" \
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

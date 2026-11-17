#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original bring-up helper; mirrors the environment tools/run-apk.sh sets on x86-64.
# Consumes AOSP build outputs (Apache-2.0). No GPL-3.0 sources.
#
# Stage what an arm64 a4l needs, beside the ART tree from tools/assemble-arm64-art.sh
# (ACL_ART_LIBC=glibc), to run an app on the real framework: the framework boot jars, a4l's two
# dex jars, framework-res.apk, a font, and a run.sh that sets the environment. Everything staged
# here is dex or data, so it comes from the x86-64 build.
#
# Usage:  tools/stage-arm64-framework.sh DEST_DIR [AOSP_ROOT]
#   AOSP_ROOT default $ACL_AOSP_ROOT or /mnt/dev/aosp
# Needs build/framework/artjars/{aclshims,hostboot}.jar, which tools/run-apk.sh writes on its
# first run on the build machine.
#
# On the target, with DEST_DIR, the ART tree (as "art") and the a4l binary in one directory R:
#   sh R/fw/run.sh R app.apk [a4l args]
set -eu
DEST="${1:?usage: stage-arm64-framework.sh DEST_DIR [AOSP_ROOT]}"
R="${2:-${ACL_AOSP_ROOT:-/mnt/dev/aosp}}"
ACLROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
JARS="$ACLROOT/build/framework/artjars"
for j in aclshims hostboot; do
  [ -f "$JARS/$j.jar" ] || { echo "missing $JARS/$j.jar (run tools/run-apk.sh once on x86-64)" >&2; exit 1; }
done
first() { for p in "$@"; do [ -e "$p" ] && { printf '%s\n' "$p"; return; }; done; }

mkdir -p "$DEST/bcp"
BCP=""
for m in framework-minus-apex framework-graphics ext telephony-common voip-common ims-common; do
  j=$(first "$R"/out/target/product/*/obj/JAVA_LIBRARIES/${m}_intermediates/javalib.jar)
  [ -n "$j" ] || { echo "missing boot jar $m (m $m)" >&2; exit 1; }
  cp -u "$j" "$DEST/bcp/$m.jar"; BCP="$BCP $m"
done
MP=$(first "$R"/out/soong/.intermediates/packages/providers/MediaProvider/apex/framework/framework-mediaprovider/*/dex/framework-mediaprovider.jar)
[ -n "$MP" ] && { cp -u "$MP" "$DEST/bcp/framework-mediaprovider.jar"; BCP="$BCP framework-mediaprovider"; }
cp -u "$JARS/aclshims.jar" "$JARS/hostboot.jar" "$DEST/"
FRES=$(first "$R"/out/target/product/*/system/framework/framework-res.apk)
[ -n "$FRES" ] || { echo "missing framework-res.apk (m framework-res)" >&2; exit 1; }
cp -u "$FRES" "$DEST/"
cp -u "$R/external/roboto-fonts/Roboto-Regular.ttf" "$DEST/"

{
  cat <<'HDR'
#!/bin/sh
# usage: run.sh ROOT app.apk [a4l args]; ROOT holds a4l, art/ and fw/
R=$1; APK=$2; shift 2
T=$R/art; F=$R/fw
B=
HDR
  for m in $BCP; do printf 'B=$B$F/bcp/%s.jar:\n' "$m"; done
  cat <<'TAIL'
B=$B$F/aclshims.jar
mkdir -p "$R/home" "$R/cache"
export ACL_ART_HOME=$T ACL_ART_BOOTCLASSPATH=$B ACL_ART_NATIVE=1 \
  ACL_ART_HOST_BOOTSTRAP=$F/hostboot.jar ACL_ART_FRAMEWORK_RES=$F/framework-res.apk \
  ACL_GRAPHICS_FONT=$F/Roboto-Regular.ttf \
  ACL_ART_GRAPHICS_LIB=$T/lib64/libacl_graphics_jni.so ACL_ART_GLES_LIB=$T/lib64/libacl_gles_jni.so \
  LD_LIBRARY_PATH=$T/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  HOME=$R/home XDG_CACHE_HOME=$R/cache
exec "$R/a4l" run "$APK" --vm=art "$@"
TAIL
} > "$DEST/run.sh"
chmod +x "$DEST/run.sh"
echo "staged $(du -sh "$DEST" | cut -f1) at $DEST"

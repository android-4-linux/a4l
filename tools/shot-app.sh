#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling; wraps tools/run-apk.sh + the acl graphics ACL_GPU_DUMP seam +
# netpbm. Grounds docs/app-corpus-status.md. No GPL-3.0 sources.
#
# shot-app.sh <apk> <out.png> <wayland-display>
#
# Run an APK on acl's zero-copy GPU present path (Skia -> Ganesh Vulkan -> tiled dma-buf) against
# ONE named compositor, and save the frame acl rendered.
#
# The compositor is an explicit argument, not the ambient session, so one invocation == one
# compositor: acl presents to <wayland-display>, and that is the only thing this touches. acl needs
# a compositor to run its present loop, but the CAPTURE does not come from the compositor — it comes
# from acl's own Skia surface via ACL_GPU_DUMP (gpu_dmabuf.cpp: readPixels of the just-flushed
# frame, written as PPM, overwritten each frame). So this works on ANY compositor, including a bare
# tinywl that advertises no screencopy/screenshot protocol — which is why we do NOT use grim.
#
# The image is exactly what acl drew (the app's surface, no desktop chrome), at the size acl
# presents (the compositor's output size, e.g. 1280x720).
#
# Env: ACL_ART_HOME (host ART build; defaults to /mnt/dev/aosp/out/host/linux-x86)
#      ACL_SHOT_SETTLE  seconds to wait for the first frame before grabbing (default 20)
set -u

ACL_ART_HOME="${ACL_ART_HOME:-/mnt/dev/aosp/out/host/linux-x86}"
export ACL_ART_HOME
SETTLE="${ACL_SHOT_SETTLE:-20}"

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <apk> <out.png> <wayland-display>" >&2
  echo "  e.g. $0 app.apk /tmp/app.png wayland-1" >&2
  exit 2
fi
APK="$1"; OUT="$2"; WD="$3"

# PPM -> PNG: prefer netpbm's pnmtopng (clean), else ImageMagick.
if command -v pnmtopng >/dev/null 2>&1; then TOPNG=pnmtopng
elif command -v magick    >/dev/null 2>&1; then TOPNG="magick"
elif command -v convert   >/dev/null 2>&1; then TOPNG=convert
else echo "shot-app: need pnmtopng or ImageMagick to write PNG" >&2; exit 3; fi

sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$WD"
[ -S "$sock" ] || { echo "shot-app: no Wayland socket at $sock (is a compositor running on $WD?)" >&2; exit 4; }

cd "$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d /tmp/acl-shot-app.XXXXXX)"
n="$(basename "$APK" .apk)"
log="$W/$n.log"; ppm="$W/frame.ppm"

# Present against THIS compositor; dump each rendered frame to $ppm. --present-frames=0 keeps the
# app up until we kill it.
( WAYLAND_DISPLAY="$WD" ACL_PRESENT_GPU=1 ACL_GPU_DUMP="$ppm" ACL_RUN_TIMEOUT=60 \
    XDG_CACHE_HOME="$W/cache" ./tools/run-apk.sh "$APK" --present-frames=0 > "$log" 2>&1 ) &
BG=$!

# Wait for the first presented frame (its dump lands in $ppm), not a blind sleep.
for _ in $(seq 1 "$SETTLE"); do
  grep -q "dmabuf present(A)" "$log" 2>/dev/null && [ -s "$ppm" ] && break
  kill -0 "$BG" 2>/dev/null || break
  sleep 1
done
sleep 1  # let one more full frame land (the dump is rewritten in place)

rc=0
if [ -s "$ppm" ]; then
  # Copy out from under acl's per-frame rewrite, then encode. A static app writes an identical
  # frame each time, so a copy is a clean frame; retry once if the copy caught a partial write.
  ok=0
  for _ in 1 2; do
    cp "$ppm" "$W/snap.ppm"
    case "$TOPNG" in
      pnmtopng) pnmtopng "$W/snap.ppm" > "$OUT" 2>/dev/null && ok=1 && break ;;
      *)        "$TOPNG" "$W/snap.ppm" "$OUT"  2>/dev/null && ok=1 && break ;;
    esac
    sleep 1
  done
  if [ "$ok" = 1 ]; then
    echo "  $WD -> $OUT ($(sed -n '2p' "$W/snap.ppm"))"
  else
    echo "  shot-app: PPM->PNG failed" >&2; rc=5
  fi
else
  echo "  no frame presented on $WD; see $log" >&2
  grep -iE "no launcher|failed:|not found|no Wayland|not rendered" "$log" | grep -viE "^ +at |native:" | head -2 >&2
  rc=6
fi

kill "$BG" 2>/dev/null
wait "$BG" 2>/dev/null
exit "$rc"

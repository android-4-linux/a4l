#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling; wraps tools/run-apk.sh + grim/swaymsg. Grounds
# docs/app-corpus-status.md. No GPL-3.0 sources.
# shot-app.sh <apk> <out.png> — run an APK on acl's zero-copy GPU present path (Skia -> Ganesh
# Vulkan -> tiled dma-buf) and capture JUST the app's surface.
#
# acl presents a fixed 1024x768 surface, but sway tiles the window to whatever the layout gives it,
# so a plain window-rect grab catches desktop around (or clips) the app. Float the window and set it
# to exactly the surface size first, then grab that rect.
set -u
export ACL_ART_HOME=${ACL_ART_HOME:-/mnt/dev/aosp/out/host/linux-x86}
APK="$1"; OUT="$2"
cd "$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d /tmp/acl-shot-app.XXXXXX)"
mkdir -p "$W/all"
n="$(basename "$APK" .apk)"; pkg="$(printf '%s' "$n" | sed 's/_[0-9]*$//')"
( ACL_PRESENT_GPU=1 ACL_RUN_TIMEOUT=40 XDG_CACHE_HOME="$W/cache" \
    ./tools/run-apk.sh "$APK" --present-frames=0 > "$W/$n.log" 2>&1 ) &
BG=$!
sleep 14
swaymsg "[app_id=\"$pkg\"] floating enable, resize set 1024 768, move position 40 40" >/dev/null 2>&1
sleep 3
geo=$(swaymsg -t get_tree | jq -r --arg id "$pkg" '.. | objects | select(.app_id? == $id) | "\(.rect.x),\(.rect.y) \(.rect.width)x\(.rect.height)"' | head -1)
if [ -n "$geo" ]; then grim -g "$geo" "$OUT" && echo "  $geo -> $OUT"; else echo "  NO WINDOW ($pkg)"; fi
wait $BG 2>/dev/null

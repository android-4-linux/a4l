#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling; wraps tools/run-apk.sh + grim. Grounds
# docs/app-corpus-status.md. No GPL-3.0 sources.
#
# shot-app.sh <apk> <out.png> <wayland-display>
#
# Run an APK on acl's zero-copy GPU present path (Skia -> Ganesh Vulkan -> tiled dma-buf) against
# ONE named compositor, and grab that compositor's output.
#
# The compositor is an explicit argument, not the ambient session: acl AND the screenshot both go
# to the same <wayland-display>, so one invocation == one compositor, deterministically. The model
# is a compositor DEDICATED to this run — a headless wlroots/tinywl (or sway/weston) sized to the
# surface — so grabbing its whole output IS the app, with no desktop around it and no window-manager
# IPC to locate a rect. Point it at your normal desktop and you will get the whole desktop.
#
# Capture is `grim`, which speaks wlr-screencopy: works on wlroots compositors (sway, tinywl, ...),
# NOT on mutter/GNOME (no wlr-screencopy — use its own screenshot D-Bus there instead). acl presents
# a 1024x768 surface, so a dedicated compositor at that size gives a pixel-exact frame.
#
# Env: ACL_ART_HOME (host ART build; defaults to /mnt/dev/aosp/out/host/linux-x86)
#      ACL_SHOT_SETTLE  seconds to wait for the first frame before grabbing (default 14)
set -u

ACL_ART_HOME="${ACL_ART_HOME:-/mnt/dev/aosp/out/host/linux-x86}"
export ACL_ART_HOME
SETTLE="${ACL_SHOT_SETTLE:-14}"

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <apk> <out.png> <wayland-display>" >&2
  echo "  e.g. $0 app.apk /tmp/app.png wayland-1   # a dedicated compositor on wayland-1" >&2
  exit 2
fi
APK="$1"; OUT="$2"; WD="$3"

command -v grim >/dev/null 2>&1 || { echo "shot-app: grim not found (needed to capture)" >&2; exit 3; }
# The compositor must actually be up on that socket, or acl connects to nothing and grim has no
# output to read. XDG_RUNTIME_DIR/<WD> is where a Wayland socket lives.
sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$WD"
[ -S "$sock" ] || { echo "shot-app: no Wayland socket at $sock (is a compositor running on $WD?)" >&2; exit 4; }

cd "$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d /tmp/acl-shot-app.XXXXXX)"
n="$(basename "$APK" .apk)"
log="$W/$n.log"

# Run acl against THIS compositor. --present-frames=0 = present until the window closes, so the
# app stays up long enough to grab; the background job is killed after the shot.
( WAYLAND_DISPLAY="$WD" ACL_PRESENT_GPU=1 ACL_RUN_TIMEOUT=60 XDG_CACHE_HOME="$W/cache" \
    ./tools/run-apk.sh "$APK" --present-frames=0 > "$log" 2>&1 ) &
BG=$!

# Wait for the first presented frame rather than a blind sleep when the log tells us; fall back to
# a fixed settle. "dmabuf present(A)" is the GPU path's first-frame marker.
for _ in $(seq 1 "$SETTLE"); do
  grep -q "dmabuf present(A)" "$log" 2>/dev/null && break
  kill -0 "$BG" 2>/dev/null || break
  sleep 1
done
sleep 1  # let that frame reach the compositor

rc=0
if kill -0 "$BG" 2>/dev/null; then
  if WAYLAND_DISPLAY="$WD" grim "$OUT" 2>"$W/grim.err"; then
    echo "  $WD -> $OUT"
  else
    echo "  grim failed on $WD: $(cat "$W/grim.err")" >&2
    echo "  (mutter/GNOME has no wlr-screencopy; use a wlroots compositor for $WD)" >&2
    rc=5
  fi
else
  echo "  acl exited before a frame was presented; see $log" >&2
  grep -iE "no launcher|failed:|not found|no Wayland" "$log" | grep -viE "^ +at |native:" | head -2 >&2
  rc=6
fi

kill "$BG" 2>/dev/null
wait "$BG" 2>/dev/null
exit "$rc"

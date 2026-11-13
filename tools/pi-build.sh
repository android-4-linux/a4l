#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original bring-up helper; docs/pi-plan.md P-A / §2a M1. Debian apt package set
# validated in a linux/arm64 Debian trixie container (the same toolchain a trixie Pi ships). No GPL-3.0 sources.
#
# M1 — build acl natively on a Raspberry Pi (arm64) running Debian/RPi-OS trixie (g++ 14, meson >=1.3).
# Run from the acl repo root on the Pi:  tools/pi-build.sh
# Then run an app:  ACL_AUDIO=pipewire build/src/shell/a4l run app.apk --vm=jvm --present
#
# Bookworm (Debian 12) note: its meson is 1.0.1 < the required 1.3 — first
#   sudo apt-get install -y python3-pip && pip3 install --break-system-packages 'meson>=1.3'
# and drop meson from the apt list below. trixie needs none of that.
set -e

if [ ! -f meson.build ]; then echo "run from the acl repo root (meson.build not found)"; exit 2; fi

SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
echo "== installing build deps (trixie) =="
$SUDO apt-get update -qq
$SUDO apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config ca-certificates default-jdk-headless \
  zlib1g-dev libfmt-dev libwayland-dev wayland-protocols libxkbcommon-dev \
  libvulkan-dev libpipewire-0.3-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libsdbus-c++-dev

echo "== configuring + building (native arm64) =="
meson setup build 2>&1 | grep -iE 'C\+\+ compiler|Host machine cpu|Run-time dep .*found: (YES|NO)|Program java' || true
meson compile -C build

echo ""
echo "a4l built: $(readlink -f build/src/shell/a4l)"
file build/src/shell/a4l 2>/dev/null | sed 's/^/  /'
./build/src/shell/a4l 2>&1 | head -4
echo ""
echo "Next: acl run <app.apk> --vm=jvm --present   (M1)"
echo "      then M2/M3 per docs/pi-plan.md (arm64 host ART via tools/assemble-arm64-art.sh)"

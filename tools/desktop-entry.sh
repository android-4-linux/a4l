#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l tooling; wraps tools/run-apk.sh. The Desktop Entry Specification
# (freedesktop.org) for what is written. No GPL-3.0 sources.
#
# Write or remove an APK's desktop entry and icon, so the desktop has a name and an icon for
# the app's window, its notifications and its launcher.
#
# Usage:   tools/desktop-entry.sh install <app.apk> [run-apk.sh args, default: --present]
#          tools/desktop-entry.sh remove  <app.apk | package>
#          (uninstall is another name for remove)
#
# install writes $XDG_DATA_HOME/applications/<package>.desktop and
# $XDG_DATA_HOME/icons/hicolor/256x256/apps/<package>.png ($HOME/.local/share without
# XDG_DATA_HOME). The entry starts the app with tools/run-apk.sh from this checkout, with
# ACL_ART_HOME as it is now and no run timeout; ACL_PRESENT_GPU and ACL_AUDIO are carried
# over when set. remove deletes both files; given the package in place of the APK it needs
# neither the APK nor a host ART build. An entry a4l did not write is left alone.
#
# No code from the APK runs: org.acl.internal.DesktopInstall reads its manifest and resources.
# Env and exit status are those of tools/run-apk.sh (77 without a host ART build).
set -u

usage() {
  printf 'usage: %s install <app.apk> [run-apk.sh args...]\n       %s remove <app.apk | package>\n' \
    "$0" "$0" >&2
  exit 2
}

ACTION="${1:-}"
APK="${2:-}"
case "$ACTION" in install|remove) ;; uninstall) ACTION=remove ;; *) usage ;; esac

# remove by package: the same rules as DesktopInstall, without the APK.
if [ "$ACTION" = remove ] && [ ! -e "$APK" ] \
    && [[ "$APK" =~ ^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$ ]]; then
  case "${XDG_DATA_HOME:-}" in
    /*) DATA="$XDG_DATA_HOME" ;;
    *) case "${HOME:-}" in
         /*) DATA="$HOME/.local/share" ;;
         *) printf 'DESKTOP: FAIL neither XDG_DATA_HOME nor HOME gives a data directory\n'
            exit 1 ;;
       esac ;;
  esac
  ENTRY="$DATA/applications/$APK.desktop"
  if [ ! -e "$ENTRY" ]; then
    printf 'DESKTOP: nothing to remove for %s\n' "$APK"
    exit 0
  fi
  if ! grep -qxF "X-A4L-Package=$APK" "$ENTRY"; then
    printf 'DESKTOP: FAIL %s exists and was not written by a4l\n' "$ENTRY"
    exit 1
  fi
  rm -f "$DATA/icons/hicolor/256x256/apps/$APK.png" && rm "$ENTRY" || exit 1
  printf 'DESKTOP: removed %s\n' "$ENTRY"
  exit 0
fi
[ -n "$APK" ] && [ -f "$APK" ] || usage
shift 2
[ "$#" -gt 0 ] || set -- --present

TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
APK=$(cd "$(dirname "$APK")" && pwd)/$(basename "$APK")

# The launch command, one argument per line; DesktopInstall quotes it for the Exec key.
EXEC=$(printf '%s\n' env "ACL_ART_HOME=${ACL_ART_HOME:-}" ACL_RUN_TIMEOUT=0)
for v in ACL_PRESENT_GPU ACL_AUDIO; do
  [ -n "${!v:-}" ] && EXEC+=$'\n'"$v=${!v}"
done
EXEC+=$'\n'$(printf '%s\n' "$TOOLS/run-apk.sh" "$APK" "$@")

ACL_DESKTOP_ACTION="$ACTION" ACL_DESKTOP_APK="$APK" ACL_DESKTOP_EXEC="$EXEC" \
  exec "$TOOLS/run-apk.sh" "$APK" --verify=off --main=org.acl.internal.DesktopInstall

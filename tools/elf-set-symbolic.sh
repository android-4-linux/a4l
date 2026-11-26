#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l tooling; the ELF64 layout and DT_FLAGS/DF_SYMBOLIC are the gABI's.
# No GPL-3.0 sources.
#
# Sets DF_SYMBOLIC in a little-endian ELF64 shared library's DT_FLAGS, in place: the library
# then resolves a symbol it defines to its own definition before any other library's.
#
# tools/stage-bionic-runtime.sh does this to the system libraries an app may carry a copy of.
# On Android such a library is in the system linker namespace and an app's libraries are in
# another, so the two copies never meet. a4l loads both into one namespace.
#
# Usage: elf-set-symbolic.sh <lib.so>    (exit 1 if the file has no DT_FLAGS to set it in)
set -euo pipefail
f="$1"

# The unsigned little-endian integer of $2 bytes at offset $1.
rd() { od -An -v -t "u$2" -j "$1" -N "$2" "$f" | tr -d ' \n'; }

[ "$(od -An -v -t x1 -N 6 "$f" | tr -d ' \n')" = "7f454c460201" ] || {
  echo "elf-set-symbolic: $f is not a little-endian ELF64 file" >&2; exit 1; }
phoff=$(rd 32 8); phentsize=$(rd 54 2); phnum=$(rd 56 2)
dyn_off=""; dyn_size=0
for ((i = 0; i < phnum; i++)); do
  ph=$((phoff + i * phentsize))
  if [ "$(rd "$ph" 4)" = 2 ]; then  # PT_DYNAMIC
    dyn_off=$(rd $((ph + 8)) 8); dyn_size=$(rd $((ph + 32)) 8)
    break
  fi
done
[ -n "$dyn_off" ] || { echo "elf-set-symbolic: $f has no PT_DYNAMIC" >&2; exit 1; }
for ((e = dyn_off; e + 16 <= dyn_off + dyn_size; e += 16)); do
  tag=$(rd "$e" 8)
  [ "$tag" = 0 ] && break            # DT_NULL
  if [ "$tag" = 30 ]; then           # DT_FLAGS
    low=$(rd $((e + 8)) 1)
    printf "\\$(printf '%03o' $((low | 2)))" |   # DF_SYMBOLIC
      dd of="$f" bs=1 seek=$((e + 8)) conv=notrunc status=none
    exit 0
  fi
done
echo "elf-set-symbolic: $f has no DT_FLAGS" >&2
exit 1

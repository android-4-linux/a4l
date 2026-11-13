#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl CI gate. No GPL-3.0 sources.
#
# Gate G0.e: a compile of acl's own code emits ZERO warnings.
#
# The tree builds at warning_level=2 with werror=false, deliberately: a subproject's warning
# must not fail a consumer build, and a newer compiler than the pinned image must not make the
# tree unbuildable for a contributor. That leaves nothing watching the count, so 39 cleared
# warnings can come back one careless patch at a time. This gate watches the count instead,
# in CI, where the compiler is pinned.
#
# Usage: check-warnings.sh <compile-log> [more-logs...]
#
# The log is the output of `meson compile`, captured with `tee` (use `set -o pipefail`, or
# `shell: bash` in a workflow step, so a compile failure still fails the step).
#
# Warnings from subprojects/ and third_party/ are NOT counted: they are upstream's to fix and
# acl pins them by sha, so the only honest response here would be to pin a different sha.
set -euo pipefail
cd "$(dirname "$0")/.."

if (( $# == 0 )); then
  echo "usage: $0 <compile-log> [more-logs...]" >&2
  exit 2
fi

# Paths whose warnings belong to someone else.
foreign_re='(^|/)(subprojects|third_party)/'

total_compiled=0
fail=0

for log in "$@"; do
  if [[ ! -r "$log" ]]; then
    echo "FAIL: cannot read compile log '$log'" >&2
    exit 1
  fi

  # A compile that did nothing produces no warnings and would pass vacuously. Ninja prints a
  # progress counter per compiled target; count those, and say so loudly if there are none.
  compiled="$(grep -cE '^\[[0-9]+/[0-9]+\]' "$log" || true)"
  total_compiled=$(( total_compiled + compiled ))

  ours="$(grep -E '(^|[ /])[^ ]*:[0-9]+:[0-9]+: warning:' "$log" | grep -vE "$foreign_re" || true)"
  if [[ -n "$ours" ]]; then
    count="$(printf '%s\n' "$ours" | wc -l)"
    echo "FAIL: $count warning(s) in acl's own code, from $log:" >&2
    printf '  %s\n' "$ours" >&2
    fail=1
  fi
done

if (( total_compiled == 0 )); then
  echo "FAIL: no compilation in $# log(s) -- nothing was built, so a pass would mean nothing." >&2
  echo "      Configure a fresh build directory, or delete the old one, and compile again." >&2
  exit 1
fi

if (( fail )); then
  echo "" >&2
  echo "The tree is kept at zero warnings. Fix the warning, or -- if it is upstream's and" >&2
  echo "unavoidable -- suppress it at the narrowest scope with a comment saying why." >&2
  exit 1
fi

echo "OK: zero warnings in acl's own code across $total_compiled compiled target(s)."

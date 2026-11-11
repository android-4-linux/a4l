#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: PROVENANCE.md, docs/clean-room.md. No GPL-3.0 sources.
#
# Gate G0, two checks:
#   1. Denylist -- the names and repository URLs of the GPL-3.0 predecessor must not appear
#      anywhere in the tree. The patterns are NOT stored here: a repository that exists to
#      exclude a project should not carry that project's name in order to grep for it. They
#      live in an untracked local file, one extended-regex per line, '#' comments allowed.
#      Point at it with ACL_PROVENANCE_DENYLIST; the default is ./.provenance-denylist
#      (gitignored). With no such file the check is SKIPPED, loudly -- a green run must never
#      be read as "the denylist passed" when there was no denylist.
#   2. Provenance comments -- every src/hosts/tests source file must carry a "Provenance:" line.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# --- 1. Denylist -----------------------------------------------------------------------
denylist="${ACL_PROVENANCE_DENYLIST:-.provenance-denylist}"

if [[ -r "$denylist" ]]; then
  patterns=()
  while IFS= read -r line; do
    [[ -z "${line// }" || "$line" == \#* ]] && continue
    patterns+=("$line")
  done < "$denylist"

  if (( ${#patterns[@]} == 0 )); then
    echo "WARN: $denylist has no patterns; reference check skipped." >&2
  else
    deny_re="$(IFS='|'; echo "${patterns[*]}")"
    # The denylist file itself is the one place the patterns are allowed to appear.
    while IFS= read -r -d '' f; do
      rel="${f#./}"
      [[ "$rel" == "${denylist#./}" ]] && continue
      if grep -InE "$deny_re" "$f" >/dev/null 2>&1; then
        echo "FAIL: denylisted reference in $rel (clean-room violation):" >&2
        grep -InE "$deny_re" "$f" >&2
        fail=1
      fi
    done < <(find . \( -path './.git/*' -o -path './build*/*' -o -path './builddir/*' \
                -o -path './subprojects/*/*' \) -prune -o -type f -print0)
    checked="${#patterns[@]} pattern(s)"
  fi
else
  echo "NOTE: no denylist at '$denylist' (set ACL_PROVENANCE_DENYLIST to enable);" >&2
  echo "      reference check SKIPPED -- only provenance comments were verified." >&2
fi

# --- 2. Provenance comments ------------------------------------------------------------
while IFS= read -r -d '' f; do
  if ! grep -q 'Provenance:' "$f"; then
    echo "FAIL: missing 'Provenance:' comment in ${f#./}" >&2
    fail=1
  fi
done < <(find src hosts tests \( -path './build*/*' \) -prune -o \
           -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \) -print0)

if (( fail )); then exit 1; fi
if [[ -n "${checked:-}" ]]; then
  echo "OK: no denylisted references ($checked); all source files carry provenance comments."
else
  echo "OK: all source files carry provenance comments (denylist not checked)."
fi

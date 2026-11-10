#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: PROVENANCE.md, docs/clean-room.md. No GPL-3.0 sources.
#
# Gate G0, two checks:
#   1. denylist — the the GPL-3.0 predecessor project name and its repo URLs must
#      not appear in code/specs. Only the policy documents are permitted to name it.
#   2. Provenance comments — every src/hosts/tests source file must carry a "Provenance:" line.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# --- 1. denylist -------------------------------------------------------------------
# Files allowed to name the forbidden project (they define/enforce the ban).
allow_re='^(\./)?(PROVENANCE\.md|README\.md|plan\.md|CONTRIBUTING\.md|docs/clean-room\.md|scripts/check-provenance\.sh|\.github/workflows/ci\.yml)$'
atl_re='the GPL-3.0 predecessor|gitlab\.com/[^ ]*android[_-]translation[_-]layer'

while IFS= read -r -d '' f; do
  rel="${f#./}"
  if [[ "./$rel" =~ $allow_re || "$rel" =~ $allow_re ]]; then continue; fi
  if grep -InE "$atl_re" "$f" >/dev/null 2>&1; then
    echo "FAIL: the predecessor reference in $rel (clean-room violation):" >&2
    grep -InE "$atl_re" "$f" >&2
    fail=1
  fi
done < <(find . \( -path './.git/*' -o -path './build*/*' -o -path './builddir/*' \
            -o -path './subprojects/*/*' \) -prune -o -type f -print0)

# --- 2. Provenance comments ------------------------------------------------------------
while IFS= read -r -d '' f; do
  if ! grep -q 'Provenance:' "$f"; then
    echo "FAIL: missing 'Provenance:' comment in ${f#./}" >&2
    fail=1
  fi
done < <(find src hosts tests \( -path './build*/*' \) -prune -o \
           -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \) -print0)

if (( fail )); then exit 1; fi
echo "OK: no denylisted references; all source files carry provenance comments."

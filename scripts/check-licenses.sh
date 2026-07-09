#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: PROVENANCE.md license policy. No GPL-3.0 sources.
#
# Gate G0: reject GPL-3.0 / LGPL-3.0 / AGPL license identifiers anywhere in the tree.
# Only license *declarations* are inspected (SPDX-License-Identifier lines and METADATA
# `license:` fields), so prose in policy docs that names these licenses does not trip it.
set -euo pipefail
cd "$(dirname "$0")/.."

# "GPL-3.0" is a substring of "LGPL-3.0" and "AGPL-3.0", so this catches all three.
forbidden='(AGPL-3\.0|LGPL-3\.0|GPL-3\.0)'
excludes=(--exclude-dir=.git --exclude-dir=build --exclude-dir='build-*'
          --exclude-dir=builddir --exclude-dir=subprojects)

# The grep patterns below embed the literal license-tag prefix; fence them so `reuse
# lint` does not parse it as this file's own (invalid) SPDX expression.
# REUSE-IgnoreStart
tag='SPDX-License''-Identifier:'
hits="$(grep -RInE "${excludes[@]}" "${tag}[^\"']*${forbidden}" . || true)"
hits+="$(grep -RInE "${excludes[@]}" "^[[:space:]]*license:[[:space:]]*\"?${forbidden}" . || true)"
# REUSE-IgnoreEnd

if [[ -n "${hits//[$'\n']/}" ]]; then
  echo "FAIL: forbidden GPL-3.0-family license identifier found:" >&2
  echo "$hits" >&2
  exit 1
fi
echo "OK: no forbidden license identifiers."

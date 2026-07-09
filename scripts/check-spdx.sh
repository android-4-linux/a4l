#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: PROVENANCE.md per-file discipline. No GPL-3.0 sources.
#
# Gate G0: every source and build file must carry an SPDX-License-Identifier in its head.
# (Markdown, .gitignore, .ini, .wrap are covered in aggregate by REUSE.toml / `reuse lint`.)
set -euo pipefail
cd "$(dirname "$0")/.."

missing=()
while IFS= read -r -d '' f; do
  if ! head -n 25 "$f" | grep -q 'SPDX-License-Identifier'; then
    missing+=("$f")
  fi
done < <(find src hosts tests tools scripts . -maxdepth 99 \
           \( -path './.git/*' -o -path './build*/*' -o -path './builddir/*' \
              -o -path './subprojects/*/*' \) -prune -o \
           -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \
              -o -name '*.h' -o -name '*.hpp' -o -name '*.c' \
              -o -name 'meson.build' -o -name 'meson.options' -o -name '*.sh' \) -print0 \
         | sort -zu)

if (( ${#missing[@]} )); then
  echo "FAIL: files missing SPDX-License-Identifier header:" >&2
  printf '  %s\n' "${missing[@]}" >&2
  exit 1
fi
echo "OK: all source/build files carry an SPDX header."

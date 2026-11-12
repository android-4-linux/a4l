#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: written for this repository.
#
# Gate G0: every third-party action is named by commit sha, with the release it came
# from in a trailing comment.
#
# A tag is a mutable pointer, and a major tag is one on purpose: actions/checkout@v7
# resolved to v7.0.0 (9c091bb) when this repository adopted it and to v7.0.1 (3d3c42e)
# the following day. Nothing announced the move, and the ref read the same before and
# after. A sha names one tree and cannot be re-pointed, so it is the only form in which
# "pinned" is a fact rather than a request. The `# vX.Y.Z` comment is required as well,
# because a bare sha records nothing about which release was actually reviewed.
set -euo pipefail
cd "$(dirname "$0")/.."

sha_re='^[0-9a-f]{40}$'
ver_re='^v[0-9]+\.[0-9]+\.[0-9]+$'
fail=0
pinned=0
local_refs=0

trim() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "$s"
}

while IFS= read -r hit; do
  file="${hit%%:*}"
  rest="${hit#*:}"
  lineno="${rest%%:*}"
  text="${rest#*:}"

  value="${text#*uses:}"
  spec="$(trim "${value%%#*}")"
  spec="${spec%\"}"
  spec="${spec#\"}"
  spec="${spec%\'}"
  spec="${spec#\'}"
  comment=""
  [[ "$value" == *"#"* ]] && comment="$(trim "${value#*#}")"

  # An action living in this repository moves with the commit that uses it.
  if [[ "$spec" == ./* ]]; then
    local_refs=$((local_refs + 1))
    continue
  fi

  ref="${spec##*@}"
  name="${spec%@*}"
  if [[ "$spec" != *@* ]] || [[ ! "$ref" =~ $sha_re ]]; then
    echo "FAIL: $file:$lineno: $spec is not pinned to a 40-character commit sha." >&2
    fail=1
    continue
  fi
  if [[ ! "$comment" =~ $ver_re ]]; then
    echo "FAIL: $file:$lineno: $name@${ref:0:12} has no trailing '# vX.Y.Z' comment" \
         "naming the release the sha was taken from." >&2
    fail=1
    continue
  fi
  pinned=$((pinned + 1))
done < <(grep -HnE '^[[:space:]]*-?[[:space:]]*uses:' .github/workflows/*.yml)

if (( fail )); then
  echo "" >&2
  echo "Pin each action by sha, e.g.:" >&2
  echo "  - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1" >&2
  echo "Resolve a tag with: gh api repos/<owner>/<repo>/commits/<tag> --jq .sha" >&2
  exit 1
fi

echo "OK: $pinned third-party action reference(s) pinned by sha" \
     "($local_refs local), each naming its release."

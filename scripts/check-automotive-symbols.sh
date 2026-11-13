#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §A01.1 (profile isolation), meson.options profile_automotive. No GPL-3.0 sources.
#
# Gate G0 (profile isolation): a TWO-SIDED nm diff proving -Dprofile_automotive
# actually gates svc/car + hosts/. The same symbol pattern must:
#   * be PRESENT in the automotive-profile build (the car/hosts modules link), and
#   * be ABSENT from the phone-profile build (they are compiled out), and
#   * be ABSENT from the shipped phone product binary (a4l).
# A one-sided "phone build has zero automotive symbols" check passes vacuously if the
# pattern is stale (renamed symbols); requiring a match on the ON side keeps it honest.
#
# The probe binary is the Tier-A unit-test executable, which references each module's
# module_name() (the automotive ones only under ACL_PROFILE_AUTOMOTIVE), so its
# exported symbols track exactly which modules the profile flag compiled in.
#
# Usage: check-automotive-symbols.sh [automotive_build_dir] [phone_build_dir]
#   defaults: build  build-phone
set -euo pipefail
cd "$(dirname "$0")/.."

auto_dir="${1:-build}"
phone_dir="${2:-build-phone}"
pattern='acl::(svc::car|hosts::)'

auto_probe="$auto_dir/tests/unit/acl_unit_tests"
phone_probe="$phone_dir/tests/unit/acl_unit_tests"
phone_product="$phone_dir/src/shell/a4l"

for f in "$auto_probe" "$phone_probe" "$phone_product"; do
  if [[ ! -f "$f" ]]; then
    echo "FAIL: expected binary not found: $f" >&2
    echo "      configure both builds first, e.g.:" >&2
    echo "        meson setup $auto_dir && meson compile -C $auto_dir" >&2
    echo "        meson setup $phone_dir -Dprofile_automotive=false && meson compile -C $phone_dir" >&2
    exit 1
  fi
done

# Positive side: the automotive build MUST export the car/hosts symbols — print them.
auto_syms="$(nm -C "$auto_probe" 2>/dev/null | grep -E "$pattern" || true)"
if [[ -z "$auto_syms" ]]; then
  echo "FAIL: automotive-profile build exports no '$pattern' symbols" >&2
  echo "      (the modules did not link, or the symbol pattern is stale)" >&2
  exit 1
fi
echo "automotive-profile build ($auto_probe) exports:"
echo "$auto_syms" | sed 's/^/  /'

# Negative side: the phone build (probe AND shipped product) MUST NOT export them.
for f in "$phone_probe" "$phone_product"; do
  if nm -C "$f" 2>/dev/null | grep -E "$pattern"; then
    echo "FAIL: phone-profile build leaks automotive symbols: $f" >&2
    exit 1
  fi
done

echo "OK: automotive symbols present under -Dprofile_automotive=true, absent under false."

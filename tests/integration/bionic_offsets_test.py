# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; plan.md §5 (Tier B), docs/ndk-scope.md (per-thread attach).
# Expected values cross-checked against AOSP bionic's public tls_defines.h (Apache-2.0). No GPL-3.0 sources.
#
# tools/gen-bionic-offsets.sh measures the STAGED bionic's internal struct layout, which the
# per-thread attach path needs to build a TCB by hand. Two things must hold, and both are cheap to
# get wrong silently:
#
#   1. With no AOSP it must SUCCEED and say so (ACL_BIONIC_OFFSETS_AVAILABLE 0), so a plain build
#      on a machine with no AOSP checkout stays green. This half runs everywhere.
#   2. With AOSP it must emit offsets that are self-consistent. A wrong offset does not fail a
#      build — it writes a bionic TCB with fields in the wrong place, i.e. memory corruption. So
#      assert the values the layout is pinned to: the slot numbers are ABI (public tls_defines.h),
#      and the TCB is exactly BIONIC_TLS_SLOTS pointers. If a future bionic renumbers a slot, this
#      fails here rather than in a GC check-failure somewhere else.
#
# Usage: bionic_offsets_test.py <source_root>
import os
import re
import subprocess
import sys
import tempfile

# name -> expected value. From AOSP bionic/libc/platform/bionic/tls_defines.h (x86_64).
EXPECTED = {
    "ACL_BIONIC_TLS_SLOTS": 10,
    "ACL_BIONIC_TLS_SLOT_SELF_": 0,
    "ACL_BIONIC_TLS_SLOT_THREAD_ID_": 1,
    "ACL_BIONIC_TLS_SLOT_STACK_GUARD_": 5,
    "ACL_BIONIC_TLS_SLOT_DTV_": 8,
    "ACL_BIONIC_TLS_SLOT_BIONIC_TLS_": 9,
    # x86_64 only. If this is ever non-zero the thread pointer is NOT the TCB address and
    # StaticTlsLayout::offset_thread_pointer() must be used instead — see the probe.
    "ACL_BIONIC_MIN_TLS_SLOT_": 0,
}

# Offsets that must exist and be non-zero (0 would mean "field not found"/miscompiled probe).
NONZERO = [
    "ACL_BIONIC_OFF_SG_STATIC_TLS_LAYOUT", "ACL_BIONIC_OFF_SG_TLS_MODULES",
    "ACL_BIONIC_OFF_TLSMODULES_MODULE_COUNT", "ACL_BIONIC_OFF_TLSMODULES_MODULE_TABLE",
    "ACL_BIONIC_SIZEOF_TLSMODULE", "ACL_BIONIC_SIZEOF_TLSDTV",
    "ACL_BIONIC_SIZEOF_PTHREAD_INTERNAL", "ACL_BIONIC_SIZEOF_BIONIC_TLS",
    "ACL_BIONIC_SIZEOF_BIONIC_TCB", "ACL_BIONIC_OFF_PTHREAD_TID",
    "ACL_BIONIC_OFF_PTHREAD_BIONIC_TLS", "ACL_BIONIC_OFF_LAYOUT_BIONIC_TCB",
]


def parse(path):
    defs = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"#define\s+(ACL_BIONIC_\w+)\s+(-?\d+)\s*$", line)
            if m:
                defs[m.group(1)] = int(m.group(2))
    return defs


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: bionic_offsets_test.py <source_root>\n")
        return 2
    gen = os.path.join(sys.argv[1], "tools", "gen-bionic-offsets.sh")
    if not os.access(gen, os.X_OK):
        sys.stderr.write(f"FAIL: {gen} missing or not executable\n")
        return 1

    work = tempfile.mkdtemp(prefix="acl-bionic-offsets-")
    ok = True
    try:
        # 1. No AOSP: must still succeed, and must say offsets are unavailable.
        out = os.path.join(work, "absent.h")
        p = subprocess.run([gen, out], env=dict(os.environ, AOSP="/nonexistent"),
                           capture_output=True, text=True, timeout=120)
        if p.returncode != 0:
            sys.stderr.write(f"FAIL: no-AOSP run must succeed, exit {p.returncode}\n{p.stderr}\n")
            ok = False
        elif parse(out).get("ACL_BIONIC_OFFSETS_AVAILABLE") != 0:
            sys.stderr.write("FAIL: no-AOSP header must set ACL_BIONIC_OFFSETS_AVAILABLE 0\n")
            ok = False
        else:
            sys.stdout.write("OK: no AOSP -> offsets unavailable, build stays green\n")

        # 2. With AOSP (skip that half when there is none — CI has no checkout).
        aosp = os.environ.get("AOSP", "/mnt/dev/aosp")
        if not os.path.isdir(os.path.join(aosp, "bionic", "libc", "private")):
            sys.stdout.write(f"(no bionic sources under {aosp}: offset checks skipped)\n")
            return 0 if ok else 1

        out = os.path.join(work, "present.h")
        p = subprocess.run([gen, out], env=dict(os.environ, AOSP=aosp),
                           capture_output=True, text=True, timeout=300)
        if p.returncode != 0:
            sys.stderr.write(f"FAIL: gen failed, exit {p.returncode}\n{p.stderr}\n")
            return 1
        defs = parse(out)
        if defs.get("ACL_BIONIC_OFFSETS_AVAILABLE") != 1:
            sys.stderr.write(f"FAIL: probe did not measure {aosp}/bionic:\n{p.stderr}\n")
            return 1

        for name, want in EXPECTED.items():
            got = defs.get(name)
            if got != want:
                sys.stderr.write(f"FAIL: {name} = {got}, expected {want} "
                                 f"(bionic ABI changed? attach would corrupt the TCB)\n")
                ok = False
        for name in NONZERO:
            if not defs.get(name):
                sys.stderr.write(f"FAIL: {name} = {defs.get(name)} (probe did not measure it)\n")
                ok = False
        # The TCB is exactly BIONIC_TLS_SLOTS pointers; anything else means the slot array grew
        # a neighbor and 'tcb + slot*8' would no longer address what we think it does.
        slots, tcb = defs.get("ACL_BIONIC_TLS_SLOTS"), defs.get("ACL_BIONIC_SIZEOF_BIONIC_TCB")
        if slots and tcb != slots * 8:
            sys.stderr.write(f"FAIL: sizeof(bionic_tcb)={tcb}, expected {slots}*8\n")
            ok = False
        # Every measured field must sit inside its struct.
        for off, size in (("ACL_BIONIC_OFF_PTHREAD_TID", "ACL_BIONIC_SIZEOF_PTHREAD_INTERNAL"),
                          ("ACL_BIONIC_OFF_PTHREAD_BIONIC_TLS", "ACL_BIONIC_SIZEOF_PTHREAD_INTERNAL"),
                          ("ACL_BIONIC_OFF_PTHREAD_STACK_TOP", "ACL_BIONIC_SIZEOF_PTHREAD_INTERNAL"),
                          ("ACL_BIONIC_OFF_TLSDTV_GENERATION", "ACL_BIONIC_SIZEOF_TLSDTV")):
            if defs.get(off) is not None and defs.get(size) and defs[off] >= defs[size]:
                sys.stderr.write(f"FAIL: {off}={defs[off]} outside {size}={defs[size]}\n")
                ok = False
        if ok:
            sys.stdout.write(f"OK: {len(defs) - 1} offsets measured from {aosp}/bionic; "
                             "slot ABI and struct bounds check out\n")
    finally:
        subprocess.run(["rm", "-rf", work], check=False)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

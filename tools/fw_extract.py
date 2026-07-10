#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original tool; plan.md §4 Phase 0 (subsetting tooling), §3 R-2 (budget),
# ADR-0002 (shim-at-Manager-boundary). No GPL-3.0 sources.
#
# frameworks/base subsetting analysis (Gate G2.c). Given a pinned AOSP source tree
# (see third_party/aosp/METADATA — android-13.0.0_r84) and a seed set of packages or
# classes, compute the transitive in-tree class dependency closure by parsing Java
# imports, report the per-package class count against a budget (R-2), and flag every
# edge that reaches the no-Binder/SystemServer denylist (ADR-3 / ADR-0002). It does
# NOT vendor anything: it sizes the subset and locates the shim boundary before code
# is pulled.
#
# Import-based closure is a first-order approximation: it captures cross-package edges
# exactly (Java requires an import for a cross-package type, java.lang aside) but not
# same-package or fully-qualified-inline references. Same-package deps never cross a
# package budget; fully-qualified denylist references ARE additionally scanned inline
# so the Binder/SystemServer boundary is not under-reported.
#
# Usage:
#   fw_extract.py --src DIR[:DIR...] --seed pkg-or-fqcn[,...]
#                 [--budget N] [--pkg-budget pkg=N[,pkg=N...]]
#                 [--denylist prefix[,prefix...]] [--json]
# Exit: 0 within budget and no denylist edges; 1 budget exceeded; 2 denylist reached.
import argparse
import json
import os
import re
import sys

PACKAGE_RE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.M)
IMPORT_RE = re.compile(r"^\s*import\s+(?:static\s+)?([\w.]+?)(\.\*)?\s*;", re.M)

# The no-Binder v1 boundary (ADR-3): a seed's closure that reaches these is either
# cut or replaced by a shim at the Manager boundary (ADR-0002).
DEFAULT_DENYLIST = [
    "com.android.server",
    "android.os.Binder",
    "android.os.IBinder",
    "android.os.ServiceManager",
    "android.os.IInterface",
]


def index_sources(roots):
    """Map fqcn -> file and package -> {fqcn}, over every .java under the roots."""
    fqcn_to_file, pkg_to_fqcns = {}, {}
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if not fn.endswith(".java") or fn == "package-info.java":
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    text = open(path, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                m = PACKAGE_RE.search(text)
                pkg = m.group(1) if m else ""
                fqcn = (pkg + "." if pkg else "") + fn[:-len(".java")]
                fqcn_to_file[fqcn] = path
                pkg_to_fqcns.setdefault(pkg, set()).add(fqcn)
    return fqcn_to_file, pkg_to_fqcns


def parse_imports(text, pkg_to_fqcns):
    """Resolve a file's imports to (single fqcns, wildcard packages)."""
    singles, wild_pkgs = set(), set()
    for name, star in IMPORT_RE.findall(text):
        if star:
            wild_pkgs.add(name)
        else:
            singles.add(name)
    return singles, wild_pkgs


def resolve(name, fqcn_set):
    """Resolve an import (possibly to a nested class) to a known top-level fqcn."""
    parts = name.split(".")
    while parts:
        cand = ".".join(parts)
        if cand in fqcn_set:
            return cand
        parts.pop()  # a.b.C.Inner -> a.b.C
    return None


def is_denied(name, denylist):
    return any(name == d or name.startswith(d + ".") or name == d
              for d in denylist)


def package_of(fqcn):
    return fqcn.rsplit(".", 1)[0] if "." in fqcn else ""


def expand_seeds(seeds, fqcn_to_file, pkg_to_fqcns):
    roots, unknown = set(), []
    for s in seeds:
        if s in pkg_to_fqcns:            # a package: every class in it
            roots |= pkg_to_fqcns[s]
        elif s in fqcn_to_file:          # a single class
            roots.add(s)
        else:
            unknown.append(s)
    return roots, unknown


def closure(seed_fqcns, fqcn_to_file, pkg_to_fqcns, denylist):
    fqcn_set = set(fqcn_to_file)
    fqcn_re = re.compile(r"\b([a-z][\w.]*\.[A-Z]\w*)\b")  # inline FQCN heuristic
    seen, denyhits = set(), []
    work = list(seed_fqcns)
    while work:
        cur = work.pop()
        if cur in seen:
            continue
        seen.add(cur)
        text = open(fqcn_to_file[cur], encoding="utf-8", errors="replace").read()
        singles, wilds = parse_imports(text, pkg_to_fqcns)
        # Explicit single imports.
        for name in singles:
            if is_denied(name, denylist):
                denyhits.append((cur, name)); continue
            tgt = resolve(name, fqcn_set)
            if tgt and tgt not in seen:
                work.append(tgt)
        # Wildcard package imports.
        for pkg in wilds:
            if is_denied(pkg, denylist):
                denyhits.append((cur, pkg + ".*")); continue
            for tgt in pkg_to_fqcns.get(pkg, ()):
                if tgt not in seen:
                    work.append(tgt)
        # Inline fully-qualified denylist references (belt-and-suspenders).
        for m in fqcn_re.findall(text):
            if is_denied(m, denylist):
                denyhits.append((cur, m))
    # De-dup denylist edges.
    denyhits = sorted(set(denyhits))
    return seen, denyhits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="colon-separated Java source roots")
    ap.add_argument("--seed", required=True, help="comma-separated packages or fqcns")
    ap.add_argument("--budget", type=int, default=0, help="max total classes (0=off)")
    ap.add_argument("--pkg-budget", default="", help="pkg=N,pkg=N per-package caps")
    ap.add_argument("--denylist", default=",".join(DEFAULT_DENYLIST))
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    roots = [p for p in args.src.split(":") if p]
    for r in roots:
        if not os.path.isdir(r):
            sys.stderr.write(f"fw_extract: not a directory: {r}\n")
            return 3
    denylist = [d for d in args.denylist.split(",") if d]
    pkg_budget = {}
    for kv in (x for x in args.pkg_budget.split(",") if x):
        k, _, v = kv.partition("=")
        pkg_budget[k] = int(v)

    fqcn_to_file, pkg_to_fqcns = index_sources(roots)
    seeds = [s for s in args.seed.split(",") if s]
    seed_fqcns, unknown = expand_seeds(seeds, fqcn_to_file, pkg_to_fqcns)
    seen, denyhits = closure(seed_fqcns, fqcn_to_file, pkg_to_fqcns, denylist)

    per_pkg = {}
    for fqcn in seen:
        per_pkg.setdefault(package_of(fqcn), 0)
        per_pkg[package_of(fqcn)] += 1
    over_pkg = {p: (n, pkg_budget[p]) for p, n in per_pkg.items()
                if p in pkg_budget and n > pkg_budget[p]}
    over_total = args.budget and len(seen) > args.budget

    report = {
        "indexed_classes": len(fqcn_to_file),
        "seed_classes": len(seed_fqcns),
        "closure_classes": len(seen),
        "unknown_seeds": unknown,
        "per_package": dict(sorted(per_pkg.items())),
        "budget": args.budget,
        "over_total_budget": bool(over_total),
        "over_package_budget": over_pkg,
        "denylist": denylist,
        "denylist_edges": [{"from": a, "reaches": b} for a, b in denyhits],
    }

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"indexed {report['indexed_classes']} classes under {roots}")
        if unknown:
            print(f"WARNING: unknown seeds (not in --src): {unknown}")
        print(f"seed={report['seed_classes']} -> closure={report['closure_classes']} classes"
              + (f" (budget {args.budget})" if args.budget else ""))
        for p, n in report["per_package"].items():
            cap = pkg_budget.get(p)
            flag = "  OVER" if cap and n > cap else ""
            print(f"  {p or '(default)'}: {n}" + (f"/{cap}" if cap else "") + flag)
        if denyhits:
            print(f"DENYLIST reached ({len(denyhits)} edge(s)) — shim at the Manager boundary:")
            for a, b in denyhits:
                print(f"  {a}  ->  {b}")
        else:
            print("denylist: clean (no Binder/SystemServer edges)")

    if denyhits:
        return 2
    if over_total or over_pkg:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

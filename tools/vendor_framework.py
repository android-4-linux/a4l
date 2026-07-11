#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original tool; ADR-0002 (vendored framework, shim-at-Manager-boundary);
# docs/framework-bootstrap.md §B; third_party/aosp/METADATA (pin). Wraps
# tools/fw_extract.py. No GPL-3.0 sources.
#
# ADR-0002 framework-vendoring orchestrator. Push-button pull of a real AOSP
# frameworks/base subset into the boot classpath's vendored layer
# (third_party/aosp/frameworks/), pinned to third_party/aosp/METADATA. Commands:
#
#   check   verify the AOSP checkout is present at the configured root
#   size    report the transitive .java closure of a seed set + denylist edges
#           (the shim boundary), sized against a budget (wraps fw_extract)
#   vendor  copy the closure into third_party/aosp/frameworks/, verify each file is
#           Apache-2.0, drop any shim of the same class (one class, one layer), and
#           rewrite MANIFEST.md. --dry-run previews without writing.
#
# The AOSP checkout is NOT vendored in-tree; create it once (see `check` output) at
# $ACL_AOSP_ROOT (default /home/joel/aosp), pinned android-13.0.0_r84. Then classes
# land in the boot classpath automatically (framework/meson.build compiles the layer).
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # sibling fw_extract
import fw_extract  # noqa: E402

TAG = "android-13.0.0_r84"  # keep in sync with third_party/aosp/METADATA
DEFAULT_AOSP_ROOT = "/home/joel/aosp"
# AOSP frameworks/base Java source roots, relative to the checkout root.
DEFAULT_SRC_REL = [
    "frameworks/base/core/java",
    "frameworks/base/graphics/java",
    "frameworks/base/media/java",
]
DEFAULT_SEED = ["android.util"]  # a small starter; grow package-by-package (R-2 budget)
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DEST = os.path.join(REPO_ROOT, "third_party", "aosp", "frameworks")
SHIM_ROOT = os.path.join(REPO_ROOT, "framework", "shim")
# REUSE-IgnoreStart  (the literal license tokens below are data, not this file's header)
APACHE_MARKERS = ("Licensed under the Apache License", "SPDX-License-Identifier: Apache-2.0")
# REUSE-IgnoreEnd


def resolve_roots(aosp_root, src_rel):
    roots = [os.path.join(aosp_root, r) for r in src_rel]
    return roots, [r for r in roots if not os.path.isdir(r)]


def aosp_hint(aosp_root):
    return (
        f"AOSP checkout not found under {aosp_root} (pin {TAG}).\n"
        f"  Create it:\n"
        f"    mkdir -p {aosp_root} && cd {aosp_root}\n"
        f"    repo init -u https://android.googlesource.com/platform/manifest -b {TAG}\n"
        f"    repo sync -c -j8 platform/frameworks/base\n"
        f"  Or point $ACL_AOSP_ROOT at an existing checkout.\n"
    )


def compute(args):
    roots, missing = resolve_roots(args.aosp_root, args.src_rel)
    if missing:
        sys.stderr.write(aosp_hint(args.aosp_root))
        sys.stderr.write("  missing source root(s): " + ", ".join(missing) + "\n")
        return None
    fqcn_to_file, pkg_to_fqcns = fw_extract.index_sources(roots)
    seed_fqcns, unknown = fw_extract.expand_seeds(args.seed, fqcn_to_file, pkg_to_fqcns)
    seen, denyhits = fw_extract.closure(seed_fqcns, fqcn_to_file, pkg_to_fqcns, args.denylist)
    return {"fqcn_to_file": fqcn_to_file, "seen": seen, "denyhits": denyhits,
            "unknown": unknown}


def cmd_check(args):
    roots, missing = resolve_roots(args.aosp_root, args.src_rel)
    if missing:
        sys.stderr.write(aosp_hint(args.aosp_root))
        return 1
    print(f"OK: AOSP checkout at {args.aosp_root} ({TAG}); {len(roots)} source root(s).")
    return 0


def cmd_size(args):
    r = compute(args)
    if r is None:
        return 1
    seen, denyhits = r["seen"], r["denyhits"]
    per_pkg = {}
    for f in seen:
        p = fw_extract.package_of(f)
        per_pkg[p] = per_pkg.get(p, 0) + 1
    budget = f" (budget {args.budget})" if args.budget else ""
    print(f"seed={sorted(args.seed)} -> closure={len(seen)} class(es){budget}")
    for p, n in sorted(per_pkg.items()):
        print(f"  {p}: {n}")
    if r["unknown"]:
        print(f"WARNING: unknown seeds (not in --src-rel): {r['unknown']}")
    if denyhits:
        print(f"DENYLIST reached ({len(denyhits)} edge(s)) — shim at the Manager boundary:")
        for a, b in denyhits:
            print(f"  {a}  ->  {b}")
    else:
        print("denylist: clean (no Binder/SystemServer edges)")
    if args.budget and len(seen) > args.budget:
        sys.stderr.write(f"OVER budget: {len(seen)} > {args.budget}\n")
        return 1
    return 0


def has_apache_header(path):
    head = open(path, encoding="utf-8", errors="replace").read(4096)
    return any(m in head for m in APACHE_MARKERS)


def layer_path(root, fqcn):
    return os.path.join(root, *fqcn.split(".")) + ".java"


def write_manifest(dest, rows):
    # REUSE-IgnoreStart  (these strings are the generated MANIFEST's header, not ours)
    out = [
        "<!-- SPDX-License-Identifier: Apache-2.0 -->",
        "<!-- Provenance: ADR-0002 vendoring audit trail. AOSP frameworks/base"
        " (Apache-2.0). No GPL-3.0 sources. -->",
        "",
        "# Vendored AOSP framework classes — audit trail",
        "",
        "One row per vendored file: the fully-qualified class, the AOSP source path it",
        "was copied from (unmodified), and the pinned tag"
        " (`third_party/aosp/METADATA`).",
        "Regenerated by `tools/vendor_framework.py vendor`.",
        "",
        "| FQCN | AOSP path | Tag |",
        "|------|-----------|-----|",
    ]
    if rows:
        out += [f"| `{fqcn}` | `{path}` | {TAG} |" for fqcn, path in sorted(rows)]
    else:
        out.append("| _(none vendored yet)_ | | |")
    out.append("")
    # REUSE-IgnoreEnd
    with open(os.path.join(dest, "MANIFEST.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))


def cmd_vendor(args):
    r = compute(args)
    if r is None:
        return 1
    seen, fqcn_to_file = r["seen"], r["fqcn_to_file"]
    if args.budget and len(seen) > args.budget:
        sys.stderr.write(f"refusing: closure {len(seen)} exceeds budget {args.budget}\n")
        return 1
    if not args.skip_license_check:
        bad = sorted(f for f in seen if not has_apache_header(fqcn_to_file[f]))
        if bad:
            sys.stderr.write("refusing: non-Apache header in: " + ", ".join(bad[:10]) + "\n")
            return 1

    copied, shims_dropped, rows = [], [], []
    for fqcn in sorted(seen):
        src = fqcn_to_file[fqcn]
        dst = layer_path(args.dest, fqcn)
        rows.append((fqcn, os.path.relpath(src, args.aosp_root)))
        shim = layer_path(SHIM_ROOT, fqcn)
        shadowed = os.path.exists(shim)
        if shadowed:
            shims_dropped.append(fqcn)
        if args.dry_run:
            copied.append(fqcn)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        copied.append(fqcn)
        if shadowed:  # one class, one layer: the vendored AOSP class supersedes its shim
            os.remove(shim)
    if not args.dry_run:
        write_manifest(args.dest, rows)

    verb = "would vendor" if args.dry_run else "vendored"
    print(f"{verb} {len(copied)} class(es) -> {os.path.relpath(args.dest, REPO_ROOT)}")
    if shims_dropped:
        v = "would drop" if args.dry_run else "dropped"
        print(f"{v} {len(shims_dropped)} shadowed shim(s): {', '.join(sorted(shims_dropped))}")
    if r["denyhits"]:
        print(f"NOTE: {len(r['denyhits'])} denylist edge(s) still need shims (run `size`).")
    return 0


def main():
    ap = argparse.ArgumentParser(description="ADR-0002 framework vendoring orchestrator")
    ap.add_argument("command", choices=["check", "size", "vendor"])
    ap.add_argument("--aosp-root", default=os.environ.get("ACL_AOSP_ROOT", DEFAULT_AOSP_ROOT))
    ap.add_argument("--src-rel", default=":".join(DEFAULT_SRC_REL),
                    help="colon-separated java roots relative to the AOSP checkout")
    ap.add_argument("--seed", default=",".join(DEFAULT_SEED),
                    help="comma-separated packages or FQCNs to vendor (+ closure)")
    ap.add_argument("--budget", type=int, default=0, help="max closure size (0=off)")
    ap.add_argument("--denylist", default=",".join(fw_extract.DEFAULT_DENYLIST))
    ap.add_argument("--dest", default=DEFAULT_DEST, help="vendored-layer destination dir")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--skip-license-check", action="store_true")
    args = ap.parse_args()
    args.src_rel = [p for p in args.src_rel.split(":") if p]
    args.seed = [s for s in args.seed.split(",") if s]
    args.denylist = [d for d in args.denylist.split(",") if d]
    return {"check": cmd_check, "size": cmd_size, "vendor": cmd_vendor}[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())

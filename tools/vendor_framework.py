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
#   plan    size every package in a plan file against its own budget, plus a combined
#           total, and fail on the first package over budget
#   pin     print the pin read out of METADATA and check everything that restates it
#           in-tree still agrees with it
#
# The pin is NOT duplicated here. The tag and the checkout path are read out of
# third_party/aosp/METADATA at import, because a constant here with a `keep in sync`
# comment beside it is not a pin -- nothing makes the two agree, and the copy that is
# wrong is the one the tooling acts on. `pin` is the gate: it re-derives the tag and
# fails if the pinned manifest's filename, that manifest's own default revision, the
# automotive pairing or the Skia pairing has drifted away from it.
#
# The AOSP checkout is NOT vendored in-tree; create it once (see `check` output) at
# $ACL_AOSP_ROOT, defaulting to METADATA's local_checkout. Then classes land in the
# boot classpath automatically (framework/meson.build compiles the layer).
import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # sibling fw_extract
import fw_extract  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
METADATA = os.path.join(REPO_ROOT, "third_party", "aosp", "METADATA")
# Keys the tooling cannot run without, as "<block>.<key>" (see read_pin).
REQUIRED_PIN = ("platform.branch", "platform.local_checkout", "platform.pinned_manifest")
_SCALAR = re.compile(r'^(\s*)([A-Za-z_][A-Za-z0-9_]*):(.*)$')
_QUOTED = re.compile(r'\s*"([^"]*)"')


def parse_metadata(path=METADATA):
    """Quoted scalars out of third_party/aosp/METADATA, keyed "<block>.<key>" inside an
    indented block and "<key>" at the top level.

    Deliberately not a YAML parser and not a dependency: five pinned strings are read
    out of a file this repo also writes by hand, and requiring the VALUE to be quoted
    is what keeps a trailing `# comment` -- which most lines in that file have -- out of
    it. An unquoted or commented-out key is therefore invisible here, which read_pin
    turns into a loud failure rather than a silent empty string.
    """
    out, block = {}, ""
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = _SCALAR.match(line.rstrip("\n"))
            if not m:  # comments, list items and blanks never match
                continue
            indent, key, rest = m.groups()
            q = _QUOTED.match(rest)
            if not indent:
                block = "" if q else key  # a bare `key:` opens a block
                if q:
                    out[key] = q.group(1)
            elif q and block:
                out[block + "." + key] = q.group(1)
    return out


def read_pin(path=METADATA):
    """The AOSP pin, or exit. One home for the tag: this file has no copy of it."""
    try:
        md = parse_metadata(path)
    except OSError as e:
        raise SystemExit(f"cannot read the AOSP pin from {path}: {e}")
    missing = [k for k in REQUIRED_PIN if not md.get(k)]
    if missing:
        raise SystemExit(
            f"{path} is missing the pin key(s) {', '.join(missing)}, or their values are\n"
            f"not double-quoted. Every tag, path and sha in that file is read from here, so\n"
            f"the tooling has no fallback to run against.")
    return md


PIN = read_pin()
TAG = PIN["platform.branch"]
DEFAULT_AOSP_ROOT = PIN["platform.local_checkout"]
# AOSP frameworks/base Java source roots, relative to the checkout root.
DEFAULT_SRC_REL = [
    "frameworks/base/core/java",
    "frameworks/base/graphics/java",
    "frameworks/base/media/java",
]
DEFAULT_SEED = ["android.util"]  # a small starter; grow package-by-package (R-2 budget)
DEFAULT_DEST = os.path.join(REPO_ROOT, "third_party", "aosp", "frameworks")
DEFAULT_PLAN = os.path.join(DEFAULT_DEST, "vendor-plan.txt")
SHIM_ROOT = os.path.join(REPO_ROOT, "framework", "shim")
# REUSE-IgnoreStart  (the literal license tokens below are data, not this file's header)
# frameworks/base is Apache-2.0 at the repo level (MODULE_LICENSE_APACHE2 / NOTICE);
# many files carry NO per-file header, so we do not require one. We only REFUSE files
# that carry a forbidden copyleft marker (a stray GPL/LGPL-3/AGPL file); everything
# else inherits the pinned repo license (third_party/aosp/METADATA + REUSE.toml).
# A copyleft token counts as a declaration only on a license-identifier line. A bare
# substring search cannot tell a real identifier from a provenance comment stating that a
# file has no GPL-3.0 sources in it, and the second is the opposite claim.
FORBIDDEN_SPDX = ("GPL-3.0", "LGPL-3.0", "AGPL")
# Full licence notices are prose, not tags, so they count wherever they appear.
FORBIDDEN_NOTICES = (
    "GNU General Public License", "GNU Affero General Public License",
    "GNU Lesser General Public License, Version 3",
)
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


def load_plan(path):
    """Parse the vendoring plan: `package [budget]` per line, # comments ignored."""
    entries = []
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        entries.append((parts[0], int(parts[1]) if len(parts) > 1 else 0))
    return entries


def cmd_plan(args):
    if not os.path.isfile(args.plan_file):
        sys.stderr.write(f"plan file not found: {args.plan_file}\n")
        return 2
    entries = load_plan(args.plan_file)
    roots, missing = resolve_roots(args.aosp_root, args.src_rel)
    if missing:
        sys.stderr.write(aosp_hint(args.aosp_root))
        return 1
    fqcn_to_file, pkg_to_fqcns = fw_extract.index_sources(roots)

    print(f"ADR-0002 vendoring plan ({args.aosp_root}, {TAG})")
    print(f"{'package':<20}{'closure':>9}{'budget':>9}{'shim-edges':>12}   status")
    print("-" * 62)
    over = False
    combined = set()
    for pkg, budget in entries:
        seed_fqcns, unknown = fw_extract.expand_seeds([pkg], fqcn_to_file, pkg_to_fqcns)
        if unknown:
            print(f"{pkg:<20}{'—':>9}{budget:>9}{'—':>12}   NOT FOUND in --src-rel")
            continue
        seen, denyhits = fw_extract.closure(seed_fqcns, fqcn_to_file, pkg_to_fqcns, args.denylist)
        combined |= seen
        flag = "OK"
        if budget and len(seen) > budget:
            flag, over = "OVER BUDGET", True
        print(f"{pkg:<20}{len(seen):>9}{budget or '-':>9}{len(denyhits):>12}   {flag}")
    print("-" * 62)
    # Combined closure seeds every package at once (dedup across packages).
    all_seed, _ = fw_extract.expand_seeds([p for p, _ in entries], fqcn_to_file, pkg_to_fqcns)
    all_seen, all_deny = fw_extract.closure(all_seed, fqcn_to_file, pkg_to_fqcns, args.denylist)
    print(f"{'COMBINED (unique)':<20}{len(all_seen):>9}{'':>9}{len(all_deny):>12}")
    print("\nshim-edges = closure edges into the no-Binder/SystemServer denylist "
          "(each needs a\nshim at the Manager boundary, not a vendored class). "
          "Size, then `vendor --seed <pkg>`.")
    return 1 if over else 0


def read_head(path):
    return open(path, encoding="utf-8", errors="replace").read(4096)


# REUSE-IgnoreStart  (the tag name below is the thing being searched for, not our header)
SPDX_TAG = "SPDX-License" "-Identifier:"
# REUSE-IgnoreEnd


def has_forbidden_license(path):
    head = read_head(path)
    for line in head.splitlines():
        before, tag, after = line.partition(SPDX_TAG)
        if tag and any(tok in after for tok in FORBIDDEN_SPDX):
            return True
    return any(m in head for m in FORBIDDEN_NOTICES)


def has_apache_header(path):
    return any(m in read_head(path) for m in APACHE_MARKERS)


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
        forbidden = sorted(f for f in seen if has_forbidden_license(fqcn_to_file[f]))
        if forbidden:
            sys.stderr.write("refusing: forbidden (copyleft) license in: "
                             + ", ".join(forbidden[:10]) + "\n")
            return 1
        headerless = sum(1 for f in seen if not has_apache_header(fqcn_to_file[f]))
        if headerless:
            print(f"note: {headerless}/{len(seen)} file(s) have no per-file header; they "
                  f"inherit the repo-level Apache-2.0 pin ({TAG}) via REUSE.toml.")

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


def cmd_pin(args):
    """Re-derive the tag from METADATA and fail on anything in-tree that disagrees.

    Four places legitimately restate the tag -- a filename, that file's own default
    revision, and two pairing fields -- because each has to be readable on its own. This
    is what makes them agree: it runs on a bare checkout (no AOSP tree needed), so it can
    gate every push rather than only a machine that has the 1145 projects synced.
    """
    rel = PIN["platform.pinned_manifest"]
    manifest = os.path.join(REPO_ROOT, rel)
    print(f"pin        {TAG}   (third_party/aosp/METADATA platform.branch)")
    print(f"checkout   {DEFAULT_AOSP_ROOT}   ($ACL_AOSP_ROOT overrides)")
    print(f"manifest   {rel}")
    bad = []

    def want(cond, detail):
        print(("  ok    " if cond else "  FAIL  ") + detail)
        if not cond:
            bad.append(detail)

    want(os.path.isfile(manifest), f"the pinned manifest exists at {rel}")
    want(os.path.basename(rel).startswith(TAG + "-"),
         f"the pinned manifest's filename names the tag ({TAG}-*)")
    revision = f'revision="refs/tags/{TAG}"'
    head = ""
    if os.path.isfile(manifest):
        with open(manifest, encoding="utf-8") as fh:
            head = fh.read(4096)  # the <remote>/<default> elements lead the file
    want(revision in head, f"the pinned manifest defaults to {revision}")
    want(PIN.get("automotive.platform_pairing") == TAG,
         f"automotive.platform_pairing is the pinned tag "
         f"(got {PIN.get('automotive.platform_pairing', '<unset>')!r})")
    skia = PIN.get("skia_pin", "")
    want(skia.endswith("@ " + TAG),
         f"skia_pin pairs external/skia with the pinned tag (got {skia or '<unset>'!r})")

    if bad:
        sys.stderr.write(
            f"\n{len(bad)} of the checks above failed. Each one covers a place that has to\n"
            f"restate the AOSP tag to be readable on its own, and they no longer agree with\n"
            f"third_party/aosp/METADATA platform.branch ({TAG}). Re-point them, or, if the\n"
            f"pin itself moved, regenerate the manifest (tools/aosp/manifests/README.md)\n"
            f"rather than editing the tag in place.\n")
        return 1
    print("OK: the AOSP tag has one home and every in-tree restatement of it agrees.")
    return 0


def main():
    ap = argparse.ArgumentParser(description="ADR-0002 framework vendoring orchestrator")
    ap.add_argument("command", choices=["check", "size", "vendor", "plan", "pin"])
    ap.add_argument("--aosp-root", default=os.environ.get("ACL_AOSP_ROOT", DEFAULT_AOSP_ROOT))
    ap.add_argument("--src-rel", default=":".join(DEFAULT_SRC_REL),
                    help="colon-separated java roots relative to the AOSP checkout")
    ap.add_argument("--seed", default=",".join(DEFAULT_SEED),
                    help="comma-separated packages or FQCNs to vendor (+ closure)")
    ap.add_argument("--budget", type=int, default=0, help="max closure size (0=off)")
    ap.add_argument("--denylist", default=",".join(fw_extract.DEFAULT_DENYLIST))
    ap.add_argument("--dest", default=DEFAULT_DEST, help="vendored-layer destination dir")
    ap.add_argument("--plan-file", default=DEFAULT_PLAN, help="plan for the `plan` command")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--skip-license-check", action="store_true")
    args = ap.parse_args()
    args.src_rel = [p for p in args.src_rel.split(":") if p]
    args.seed = [s for s in args.seed.split(",") if s]
    args.denylist = [d for d in args.denylist.split(",") if d]
    return {"check": cmd_check, "size": cmd_size, "vendor": cmd_vendor,
            "plan": cmd_plan, "pin": cmd_pin}[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())

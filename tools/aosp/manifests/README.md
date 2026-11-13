<!--
SPDX-License-Identifier: Apache-2.0
Provenance: a4l-authored bookkeeping. No GPL-3.0 sources.
-->

# The AOSP pin, as a manifest

`acl` builds its runtime against a local AOSP checkout at `$ACL_AOSP_ROOT`
(`third_party/aosp/METADATA:local_checkout`). Until now the repository recorded
*which release* that was but had no way to state *which tree*: a 64 GB checkout
is not publishable, and `repo init -b android-13.0.0_r84` resolves differently
depending on when it runs, because a release tag pins project revisions only for
as long as nobody re-points it.

`android-13.0.0_r84-pinned.xml` closes that. It is `repo manifest -r` output
from the checkout a4l is developed against, recording all **1145** projects at
an explicit 40-character commit sha rather than at a branch or tag name.

## Recreating the tree

```sh
mkdir aosp && cd aosp
repo init -u https://android.googlesource.com/platform/manifest \
          -b android-13.0.0_r84
cp <acl>/tools/aosp/manifests/android-13.0.0_r84-pinned.xml \
   .repo/manifests/pinned.xml
repo init -m pinned.xml
repo sync -c -j8          # ~64 GB; -c limits to the current branch
```

A partial sync is enough for most a4l work; `METADATA` lists the components that
are actually consumed.

The `repo init -u` step is not optional, and dropping the file in afterwards is
not a convenience: the manifest's remote is `fetch=".."`, relative to wherever
the manifest repository itself was cloned from. The file cannot be used
standalone or from another location, because nothing in it says what it is
relative *to*.

## The manifest alone does not reproduce a4l's build tree

Two projects are **modified in the working tree**, deliberately, and those edits
are not commits, so no manifest can carry them. Apply them after syncing:

| Project | Patch |
|---|---|
| `libcore` | `tools/aosp/patches/libcore-host-musl-sigrtmin.patch` |
| `external/skia` | `tools/aosp/patches/external-skia-host-vulkan.patch` |

This is the same hazard the libcore patch itself was in before it was adopted:
an uncommitted edit in a tree where `repo sync` discards it without comment.
The pin and the patches are kept in one repository for that reason — a pin that
needs patches stored somewhere else is a pin that quietly does not reproduce.

The two Chromium patches in `tools/aosp/patches/` apply to a Chromium checkout,
not to this tree.

## Verified on 2026-11-11

Against the checkout at `/mnt/dev/aosp`:

- **1145 of 1145** projects in the generated manifest are present locally.
- **0** projects carry local commits: every project's `HEAD` equals its own
  `refs/tags/android-13.0.0_r84^{}`. That matters because a manifest pinning a
  sha that exists only on this machine cannot be fetched by anyone else, which
  would make the pin unusable rather than merely imprecise.
- **3** modified files, in the 2 projects above, and both patches
  `git apply --check -R` clean, so the patch files and the working tree agree
  byte for byte.
- The recipe above was **run**, not just written: a scratch `repo init` followed
  by `repo init -m android-13.0.0_r84-pinned.xml` is accepted, and `repo
  manifest -o -` then resolves `libcore` to `df86e79006af` and `external/skia`
  to `5e242c23f24e`, matching this machine's checkout and the patch headers.

## A note on the two shas for one tag

`android-13.0.0_r84` is an **annotated** tag, so it has two shas, and
`METADATA` now records both:

| | sha |
|---|---|
| tag object | `05bc4f23cd577692d879cb1ab7ec1c0a583d2f83` |
| commit it points at | `45b33a9a1241420dd45e562da0798f3f8e971f3b` |

`git rev-parse refs/tags/<tag>` returns the first; `rev-parse
refs/tags/<tag>^{}` returns the second. Comparing one against the other reads
as a mismatch when nothing is wrong, which is worth stating once here rather
than rediscovering per project.

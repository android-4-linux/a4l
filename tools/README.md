<!-- SPDX-License-Identifier: Apache-2.0 -->
# tools/

Repository tooling. Phase 0 placeholder for the items called out in `plan.md` §4 Phase 0:

- **frameworks/base subsetting/extraction** — given a pinned AOSP tree and a target API level,
  extract the curated `frameworks/base` package subset (view, graphics, content, os, app, util,
  text, widget) into `third_party/aosp/`, measuring vendored class count per package to hold the
  R-2 budget (shim-at-Manager-boundary rule, ADR-0002).
- **AOSP mirror pinning** — resolve and record repo-manifest pins (platform tag, automotive car
  tag, Skia milestone) into `third_party/*/METADATA`.
- **app-corpus fetch** — resolve the hash-pinned F-Droid corpus manifest into
  `tests/app-corpus/apks/` (gitignored).

These land as they are needed; nothing here is on the critical path for the Phase 0 skeleton
build. Scripts added here follow the same SPDX + provenance + DCO rules as the rest of the tree.

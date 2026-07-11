<!-- SPDX-License-Identifier: Apache-2.0 -->
# third_party/

Vendored external dependencies, pinned and (where practical) unmodified. Governed by
`PROVENANCE.md`.

**Sources are not committed wholesale** (see `/.gitignore`) — tooling fetches them by the
pin recorded here. What *is* tracked per subtree:
- `METADATA` — origin URL, tag/branch, commit SHA, license (SPDX), and any pinning notes.
- upstream `LICENSE` — carried verbatim.
- local patches, if any, under `<dep>/patches/`.

Each dependency's build system is its own (Skia = GN, AOSP = Soong); acl consumes the
resulting prebuilt artifacts. Nothing here may carry a forbidden license
(GPL-3.0 / LGPL-3.0 / AGPL — CI rejects those SPDX ids).

## Present subtrees
- `aosp/` — ART, `frameworks/base` subset, libandroidfw, libziparchive, bionic, CTS,
  `packages/services/Car`. Apache-2.0 / BSD. Not yet vendored — METADATA template only.
- `skia/` — 2D rasterization (BSD-3). Not yet vendored — METADATA template only. See A02.2
  for the single-pinned-Skia rule.

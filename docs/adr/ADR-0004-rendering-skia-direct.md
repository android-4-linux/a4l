<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0004: Rendering — Skia-direct via wcs canvases; hwui dropped

- **Status:** Accepted — supersedes the plan's original ADR-4 hwui-vs-Skia spike
- **Date:** 2026-07-09
- **Provenance:** plan.md §3 ADR-4, Addendum A02.1–A02.2, R-5. No GPL-3.0 sources.

## Context
AOSP renders Views via hwui (its own Skia-based renderer with RenderThread/damage/HW layers),
which has deep `gui/` (BufferQueue/SurfaceFlinger) coupling. Addendum A02 commits all native
windowing to **wcs** bindings and all rasterization to **Skia** — no GTK/Qt, no hard Flutter dep.

## Decision
**Skia-direct is committed; the hwui-vs-Skia spike is canceled and hwui is not vendored.**
`android.graphics` JNI (already a thin Skia wrapper in AOSP) re-targets host Skia via a shim.
Render backends are the wcs Skia canvases only: `skia-shm` (guaranteed), `skia-egl`,
`skia-vulkan-dmabuf` (preferred on RK3588/SA8155P/RADV).

## Consequences
We re-own natively: a RenderThread-equivalent (one render thread per toplevel, Choreographer on
frame callbacks + `wp_presentation`), buffer-age damage, and the text stack (Skia + HarfBuzz/ICU/
fontconfig; vendor AOSP `minikin` (Apache-2.0) if CTS text metrics miss — RA-7). **Skia has no
stable API:** a single pinned Skia build is shared with wcs (no ODR duplication); pin to the
milestone matching the vendored `frameworks/base` tag, recorded in `third_party/*/METADATA`
(RA-6). wcs upstream work lands on the critical path: dmabuf-feedback-v4 + syncobj bindings.

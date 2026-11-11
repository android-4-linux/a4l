# Meson subprojects

Meson `.wrap` files declaring how dependencies are located or fetched. Wraps are tracked;
the checked-out sources they produce are gitignored (see `/.gitignore`).

## Two classes of dependency

**System / small deps** — resolved via `dependency()` with a wrap fallback (e.g. Catch2).
Fetched from Meson WrapDB by `meson wrap install <name>` or on first `meson setup`.

**Heavy vendored deps (Skia, AOSP ART/frameworks)** — NOT built by Meson. They keep their
own build systems (Skia = GN, AOSP = Soong) and are consumed as **prebuilt imported
artifacts**. When `gfx/` lands, Skia is wired in via a `gfx/FindSkia`-style helper (or a
`.wrap` pointing at the prebuilt tree once its location is fixed); the authoritative
origin/tag/SHA/license lives in `third_party/skia/METADATA`. No placeholder `.wrap` is
committed for it, because Meson parses every `.wrap` in this directory eagerly and a
sectionless stub fails configuration. The single-pinned-Skia rule (A02.2) is recorded in
`third_party/skia/METADATA`.

## Dependencies
- **Catch2** (`catch2.wrap`, Tier A unit tests, BSL-1.0) — pinned WrapDB wrap (v3.14.0),
  fetched on first `meson setup`. The test target is `required: false`, so an offline setup
  that cannot fetch it disables the tests with a warning rather than failing. Refresh with
  `meson wrap update catch2`.
- **wayland-cxx-scanner** (`wayland-cxx-scanner.wrap`, wm/ Wayland bindings, MIT) — pinned
  `wrap-git` at tag **v1.0.1**. Only reached when neither `-Dwcs_root=` nor a
  `../wayland-cxx-scanner` sibling checkout resolves, so a wcs developer keeps editing their
  own checkout and everyone else gets the pin. `subproject()` is `required : false`: an
  offline setup disables the real presenter rather than failing. Bump the `revision`
  deliberately — it is never a branch.
- **pugixml** (`pugixml.wrap`, BSD-2-Clause) — wcs's XML parser, not acl's. Meson resolves a
  nested subproject's wrap from the **top-level** project, so the wrap has to live here for
  the wcs subproject to build on a machine without a system pugixml. Pinned WrapDB wrap
  (v1.15); refresh with `meson wrap update pugixml`.
- **drm-cxx** (`drm-cxx.wrap`, KMS scanout present sink, MIT) — resolved via
  `dependency('drm-cxx', fallback : ...)`, `required : false`. Normally a local sibling
  checkout symlinked in as `subprojects/drm-cxx` (gitignored); the wrap points at the git
  remote for a clean checkout.
- **Skia** — not a Meson subproject; consumed as a prebuilt (A02.2). `-Dskia_prefix=`, else
  `$ACL_SKIA_PREFIX` and the conventional install locations are searched. Build one with
  wcs's `scripts/build-skia.sh` at the shared pin. See `third_party/skia/METADATA`.

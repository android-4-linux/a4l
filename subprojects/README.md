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
- **Skia** — not a Meson subproject; consumed as a prebuilt (A02.2). See
  `third_party/skia/METADATA`.

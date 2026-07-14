<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: Android android.graphics.Typeface / android.graphics.fonts + SystemFonts
     public surface (AOSP framework api-33, Apache-2.0); Skia SkTypeface/SkFontMgr docs. No GPL-3.0 sources. -->

# Fonts — system font map, families, and the `/system/etc/fonts.xml` gap

## The symptom

Every host-ART run logs:

```
SystemFonts E java.io.FileNotFoundException: /system/etc/fonts.xml: open failed: ENOENT
```

This is **benign**. `android.graphics.Typeface.<clinit>` calls
`SystemFonts.getSystemPreinstalledFontConfig()`, which reads Android's font config to build
the system `Typeface` map. On a Linux host there is no `/system`, so it throws — caught inside
`SystemFonts`, leaving the framework font map empty.

## Why you can't "map a base directory"

`SystemFonts` hardcodes the paths as `static final` constants and passes them straight into
`getSystemFontConfigInternal("/system/etc/fonts.xml", "/system/fonts/", …)`. There is **no
`ANDROID_ROOT` indirection** — it always looks at literal `/system`. So the only ways to satisfy
the *framework* path are (a) put real files at `/system` (a user mount-namespace `mount --bind`,
or shipping them on the embedded/automotive image where acl owns the rootfs), or (b) reflectively
override the constants (fragile — `String` constants inline at the use-site).

## What acl does instead — own it in Skia

acl does not use `SystemFonts`. `graphics_jni` is backed by Skia; `HostBootstrap.seedTypefaces()`
(run once after the graphics natives register) builds `Typeface.DEFAULT` from a real font file
(`ACL_GRAPHICS_FONT` → Android's `Roboto-Regular.ttf`, else a probed host font) and installs it as
the default + the `sSystemFontMap` family aliases. That is why text already renders. Fonts are
**probed at runtime** (host `/usr/share/fonts`, the AOSP tree) — no in-tree font vendoring, and no
`/system` dependency.

`seedTypefaces()` also maps distinct **serif** and **monospace** families (plus aliases: arial,
helvetica, georgia, courier, monaco, …) and probes real serif/mono files, with the
`ACL_FONT_SERIF` / `ACL_FONT_MONO` env vars as an override hook. Today those distinct families
**gracefully fall back to the sans default** — see the blocker below.

## Why distinct families need a native entry point

Loading a *second* font file into a `Typeface` from Java does not work on this host:

- **`Typeface.createFromFile(path)` fails silently** — its internal `Font.Builder(File)` path
  returns `Typeface.DEFAULT` rather than a distinct face.
- **Driving the low-level `Font$Builder` / `FontFamily$Builder` natives reflectively SEGFAULTs** —
  those framework natives are `@CriticalNative` (no `JNIEnv*`/`jclass`, raw calling convention),
  and `graphics_jni` compiles them with that ABI (see `docs/render-host-scope.md:285`), so a
  standard reflective JNI invocation crashes the process.

So distinct serif/mono/fallback faces are produced in **native** code.

## The native entry — DONE (option 1)

`graphics_jni` (`vendor/acl/graphics_jni/`, rebuilt with `m libacl_graphics_jni`) exports a
**non-`@CriticalNative`** C entry `acl_typeface_from_path(const char*) -> jlong` (a reffed
`SkTypeface*` handle from `SkTypeface::MakeFromFile`). The runtime (`load_acl_graphics`) `dlsym`s
it and binds it as `org.acl.internal.HostBootstrap.nTypefaceFromPath(String)`;
`seedTypefaces().firstTypeface()` calls it and wraps the handle with `new Typeface(long)`
(`free_typeface` unrefs it). Verified: serif (`NotoSerif`) and monospace (`AdwaitaMono`) now load
as distinct faces, no segfault.

## Remaining (option 2) — full fonts.xml fallback collection

The single-`SkTypeface`-per-family model has no **per-script glyph fallback** (CJK, Arabic, …) or
**emoji** — that is minikin/SkShaper's job. The complete answer is `graphics_jni` building a real
`SkFontMgr_Custom` / font collection from a vendored `fonts.xml`
(`frameworks/base/data/fonts/fonts.xml`, Apache-2.0) + a probed/vendored font set, so text with
mixed scripts resolves glyph-by-glyph. Left as future work; the family-map + probing seam is wired.

## Verify

```sh
# any --vm=art run triggers seedTypefaces; look for the load logs:
bash run_apk.sh <app.apk> 2>&1 | grep -E 'loaded typeface|seedTypefaces'
```

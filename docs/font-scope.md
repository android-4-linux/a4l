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

## Blocker: distinct families need a native entry point

Loading a *second* font file into a `Typeface` from Java does not work on this host:

- **`Typeface.createFromFile(path)` fails silently** — its internal `Font.Builder(File)` path
  returns `Typeface.DEFAULT` rather than a distinct face.
- **Driving the low-level `Font$Builder` / `FontFamily$Builder` natives reflectively SEGFAULTs** —
  those framework natives are `@CriticalNative` (no `JNIEnv*`/`jclass`, raw calling convention),
  and `graphics_jni` compiles them with that ABI (see `docs/render-host-scope.md:285`), so a
  standard reflective JNI invocation crashes the process.

So distinct serif/mono/fallback faces must be produced in **native** code.

## The native fix (next step)

Add to `graphics_jni` (`vendor/acl/graphics_jni/`, rebuilt with `m libacl_graphics_jni`) a clean
**non-`@CriticalNative`** entry — either:

1. `nAclTypefaceFromPath(String path) -> long` returning a reffed `SkTypeface*` handle that
   `new Typeface(long)` wraps (minimal — `seedTypefaces.firstTypeface()` swaps `createFromFile`
   for this one call), or
2. **fonts.xml parsing**: `graphics_jni` builds a real `SkFontMgr_Custom` / font collection from a
   vendored `fonts.xml` (`frameworks/base/data/fonts/fonts.xml`, Apache-2.0) + a probed/vendored
   font set, giving proper **per-script fallback** (CJK, Arabic, …) and **emoji** coverage — which
   the single-`SkTypeface` path cannot do (glyph fallback is minikin/SkShaper's job).

Option 2 is the complete answer (families + fallback chain + emoji); option 1 is the quick win for
distinct sans/serif/mono. `seedTypefaces()` already has the family-map + probing seam wired, so the
native call drops straight in.

## Verify

```sh
# any --vm=art run triggers seedTypefaces; look for the load logs:
bash run_apk.sh <app.apk> 2>&1 | grep -E 'loaded typeface|seedTypefaces'
```

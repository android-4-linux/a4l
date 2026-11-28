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
or shipping them on the embedded/automotive image where a4l owns the rootfs), or (b) reflectively
override the constants (fragile — `String` constants inline at the use-site).

## What a4l does instead — own it in Skia

a4l does not use `SystemFonts`. `graphics_jni` is backed by Skia; `HostBootstrap.seedTypefaces()`
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

## A character the typeface does not have

A Paint draws with one typeface. A character it has no glyph for used to come out as the
font's missing-glyph box: the katakana middle dot (U+30FB) an app puts between two words,
in a UI font that is Latin only. Android takes such a character from another of the system's
fonts.

`graphics_jni` now cuts text into runs by font (`for_each_font_run`): the Paint's typeface,
and for a character it has no glyph for, the first fallback font that has one. Measuring and
drawing go by the same runs, and `Paint.hasGlyph` answers for one character from both.

The fallback fonts are looked for in this order, each loaded when it is first needed:

| Source | |
|---|---|
| `ACL_FONT_FALLBACK` | font files, separated by colons |
| the host's fontconfig | the font it likes best for the character, sans-serif where it has the choice |

fontconfig is found at run time (`libfontconfig.so.1`); a host without it has only the named
files. ASCII text is the Paint's without a look.

Limits:

- There is no shaping. A script that needs it (Arabic, the Indic scripts) gets its glyphs,
  not its ligatures or reordering.
- A fallback font is used as it is: no bold or italic is made for it where the Paint's
  typeface is bold or italic.
- Line metrics are the Paint's typeface's alone.
- A character that is meant to be invisible and that no font has (a variation selector) is
  still drawn as the Paint's typeface draws a missing glyph.
- The first character that needs it loads the host's font configuration on the thread that
  asked.

A full `fonts.xml` collection, with per-language preference, is still not built.

## Verify

```sh
# any --vm=art run triggers seedTypefaces; look for the load logs:
bash run_apk.sh <app.apk> 2>&1 | grep -E 'loaded typeface|seedTypefaces'
```

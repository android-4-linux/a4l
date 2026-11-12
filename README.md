<!-- SPDX-License-Identifier: Apache-2.0 -->
# acl

A clean-room, **C++23-first** Android compatibility layer: it runs unmodified Android APKs as
native Wayland clients on desktop and embedded Linux, comparable in function to
the GPL-3.0 project that preceded it while sharing **none** of its source. Output license
is **Apache-2.0**. The automotive profile (media apps + Car App Library templates) is a
first-class target and reaches a useful milestone ahead of the phone/View path.

> **Status: unmodified APKs render.** Of 18 F-Droid apps run headless on `acl run --vm=art`, 15
> reach layout and 3 fail; reaching layout means the app booted without an `onCreate` crash and its
> decor measured and laid out, which is not a pixel check. Rendering is verified on screen for
> Jetpack Compose (`SVS.pdfinspector`), Views/Material (`opendrummer`, `a2dp.Vol`,
> `batterytemperature`) and a custom-View clock (`octalsuntime`). A real app needs
> `ACL_PRESENT_GPU=1`, the zero-copy path that draws through Skia (Ganesh Vulkan) into a dma-buf
> with a tiled format modifier; the default shm path can only present acl's own Bitmap shim. CI
> enforces the clean-room and license discipline.
>
> Not built, and none of them small: `SurfaceView` (a GL, video or camera app's region shows
> through as a hole), `WebView`, and the legacy `gles_jni` EGL bindings that `GLSurfaceView`
> initializes. `docs/app-corpus-status.md` tracks the corpus app by app. The authoritative design
> is [`plan.md`](plan.md); contributor guidance is in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Read these first

- [`plan.md`](plan.md) — full design, ADRs, phases, risk register (source of truth).
- [`PROVENANCE.md`](PROVENANCE.md) — **enforced** source/license policy. The overriding rule:
  never open the GPL-3.0 predecessor's tree; take the Android API surface from Apache-2.0 AOSP
  (AOSP-down, not GPL-across).
- [`docs/clean-room.md`](docs/clean-room.md) — the clean-room protocol and current posture.
- [`docs/adr/`](docs/adr/) — architecture decision records.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — SPDX + provenance + DCO requirements.

## Build

Build system is **Meson** (native to the Wayland/PipeWire/Mesa stack this integrates against).
Heavy vendored deps keep their own builds — **Skia via GN, AOSP ART/frameworks via Soong** — and
are consumed as prebuilt imported artifacts; the Phase 0 skeleton needs none of them.

```sh
meson setup build
meson compile -C build
./build/src/shell/acl           # prints version + linked module roster

meson test -C build                # Tier A unit tests (needs: meson wrap install catch2)
```

Automotive profile is **ON by default**. Build the phone subset (must emit zero automotive
symbols) or cross-compile for a board:

```sh
meson setup build-phone -Dprofile_automotive=false
meson setup build-rk3588 --cross-file cross/rk3588.ini
```

## Layout

```
src/{runtime,pkg,wm,gfx,input,audio,media,svc,ndk,shell}   native core (C++23)
src/svc/car/                                                vehicle property hub (automotive)
hosts/{cal,media}                                           Car App Library + media bridges (automotive)
third_party/{aosp,skia}                                     vendored, pinned, prebuilt-consumed
specs/                                                      sanitized functional specs (clean-room)
tests/{unit,integration,cts-runner,app-corpus}             Tier A/B + CTS oracle + real-app corpus
docs/adr/                                                   architecture decision records
scripts/, .github/workflows/                                CI license/provenance/SPDX gates (Gate G0)
```

## License

Apache-2.0. Copyright 2026 Contributors to acl-cxx, stated in [`NOTICE`](NOTICE).

[`LICENSE`](LICENSE) holds the license text verbatim, because automated license
detection matches that file by content rather than by what it claims; [`LICENSES/`](LICENSES/)
holds the same text under its SPDX identifier, where the [REUSE](https://reuse.software)
specification looks for it. Every file in the tree declares its own license with an SPDX
tag, `REUSE.toml` records the copyright holder for files that cannot carry a header, and
`reuse lint` verifies that nothing is unaccounted for.

<!-- SPDX-License-Identifier: Apache-2.0 -->
# acl

A clean-room, **C++23-first** Android compatibility layer: it runs unmodified Android APKs as
native Wayland clients on desktop and embedded Linux, comparable in function to
`the GPL-3.0 predecessor` (the predecessor) but sharing **none** of the predecessor's GPL-3.0 source. Output license
is **Apache-2.0**. The automotive profile (media apps + Car App Library templates) is a
first-class target and reaches a useful milestone ahead of the phone/View path.

> **Status: Phase 0 (skeleton).** This tree currently builds a stub executable and enforces the
> clean-room / license discipline in CI. Real subsystems land per the phase plan. The
> authoritative design is [`plan.md`](plan.md); day-to-day orientation is in
> [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Read these first

- [`plan.md`](plan.md) — full design, ADRs, phases, risk register (source of truth).
- [`PROVENANCE.md`](PROVENANCE.md) — **enforced** source/license policy. The overriding rule:
  never open the predecessor's tree; take the Android API surface from Apache-2.0 AOSP (AOSP-down, not
  GPL-across).
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

Apache-2.0 — see [`LICENSE`](LICENSE) and [`LICENSES/`](LICENSES/). The tree is
[REUSE](https://reuse.software)-compliant (`reuse lint`).

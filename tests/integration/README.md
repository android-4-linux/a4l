# Tier B — headless integration tests

Per `plan.md` §5. Headless integration over a Wayland socket-pair harness (reuse the
wcs Tier C live-dispatch gate design): drive the native core against an in-process
compositor stub, no display server required. Lands alongside the `wm/` + `input/` work
in Phase 2.

Scope examples: activity lifecycle transitions, surface commit/frame-callback ordering,
input event translation, clipboard/data-device round-trips.

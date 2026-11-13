<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original acl pull-request template. No GPL-3.0 sources. -->

## What this changes, and what it fixes

<!-- Lead with the finding, not the change: what is wrong today, how you know, and what the fix is.
     Measurements beat adjectives — a number, a log line, a command someone else can run. -->

## How it was verified

<!-- Name what you ran and what it printed. "Tests pass" is not a measurement; the test names and
     the counts are. If you could not verify something, say which part and why. -->

## Checklist

- [ ] Every commit is `Signed-off-by` the author (`git commit -s`) — the `dco` job checks it, and
      a commit made through the GitHub web editor now gets the trailer automatically.
- [ ] New source files carry an `SPDX-License-Identifier` and a `Provenance:` line
      (`scripts/check-spdx.sh`, `scripts/check-provenance.sh`).
- [ ] Nothing here was taken from, or written while reading, the GPL-3.0 project that preceded acl.
      The Android API surface comes from Apache-2.0 AOSP — see `PROVENANCE.md`.
- [ ] `clang-format` is a fixed point at the pinned version (`.clang-format-version`).
- [ ] Any third-party GitHub action is pinned to a full 40-character sha with a `# vX.Y.Z` comment
      (`scripts/check-action-pins.sh`).
- [ ] Status claims added to `README.md` or `docs/` carry a date, and cite a commit where a reader
      could re-measure them.

<!-- Gates run in CI, but running them locally first is faster than a round trip:
       scripts/check-licenses.sh && scripts/check-provenance.sh && scripts/check-spdx.sh \
         && scripts/check-action-pins.sh && reuse lint
-->

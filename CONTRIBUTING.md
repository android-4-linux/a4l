<!-- SPDX-License-Identifier: Apache-2.0 -->
# Contributing to acl

Before anything else, read `PROVENANCE.md` and `docs/clean-room.md`. The clean-room and
license rules are hard constraints enforced in CI, not guidelines. How people are expected to
treat each other here is in [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md); reporting an app that
does or does not run goes through the app-report issue form.

## The three per-contribution requirements

1. **SPDX header** on every file:
   ```
   // SPDX-License-Identifier: Apache-2.0
   ```
   (use the comment syntax of the file's language). Enforced by `scripts/check-spdx.sh` and
   `reuse lint`.

2. **Provenance comment** on every `src/`, `hosts/`, and test source file, naming the sources
   it was written from and ending in `No GPL-3.0 sources.`:
   ```
   // Provenance: specs/<module>.md, AOSP <path> docs, CTS <suite>. No GPL-3.0 sources.
   ```
   Enforced by `scripts/check-provenance.sh` (which also rejects denylisted references anywhere).

3. **DCO sign-off** on every commit — certify the [Developer Certificate of
   Origin](https://developercertificate.org/) by committing with `-s`:
   ```
   git commit -s -m "..."
   ```
   which appends `Signed-off-by: Your Name <you@example.com>`.

   The one exemption is Dependabot, which cannot put a trailer in its own commits. The
   gate skips a commit only when the pull request was opened by `dependabot[bot]` *and*
   that commit is authored by it; a commit of yours in a Dependabot branch is still
   checked, and claiming its author identity on a pull request of your own does not
   exempt you.

## License rules (see PROVENANCE.md for the full matrix)

- Permitted: Apache-2.0 / BSD / MIT / ISC / Zlib / BSL-1.0 statically; LGPL-2.1 and MPL-2.0
  **dynamically only**.
- Forbidden anywhere: **GPL-3.0 / LGPL-3.0 / AGPL**. `scripts/check-licenses.sh` fails the
  build on these SPDX identifiers.
- New third-party code goes under `third_party/<name>/` with a `METADATA` file and upstream
  `LICENSE`. It is not vendored wholesale into git (see `.gitignore`); tooling fetches it.

## Build & test (Meson)

```
meson setup build
meson compile -C build
meson test -C build          # requires Catch2: meson wrap install catch2
```

Cross-compile for a board target:
```
meson setup build-rk3588 --cross-file cross/rk3588.ini
```

Verify the phone-profile subset emits zero automotive symbols:
```
meson setup build-phone -Dprofile_automotive=false && meson compile -C build-phone
```

## Formatting (clang-format)

`.clang-format` is ivi-homescreen's config kept byte-identical (Chromium style at 80
columns), which is why its attribution is a `REUSE.toml` annotation rather than a header
inside the file. The version the tree is formatted with lives in `.clang-format-version`,
and CI checks the tree at exactly that version:

```
pipx install "clang-format==$(cat .clang-format-version)"
git ls-files -z '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' | xargs -0 clang-format -i
```

Three things to know before trusting a single pass:

- **It does not converge in one pass.** Trailing-comment alignment oscillates and settles
  on the second pass. Run the command above until `git diff` comes back empty.
- **A `clang-format off` fence takes no trailing text.** `// clang-format off -- why` is
  not recognized and silently does nothing. Three macro-invocation tables in
  `src/ndk/bionic_runtime.cpp` carry the bare form because clang-format repacks them
  differently on every pass.
- **The version is part of the contract.** clang-format 18 -- what `apt-get install
  clang-format` gives on an Ubuntu runner -- disagrees with the committed tree in 32
  places across 5 files; 23.1.5 agrees with it exactly. The CI job asserts the version it
  actually got, so a mismatched formatter fails loudly instead of re-flowing the tree.

Reformat-only commits belong in `.git-blame-ignore-revs`. Point git at it once:

```
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

## Local pre-flight (same checks CI runs)

```
reuse lint
scripts/check-spdx.sh
scripts/check-licenses.sh
scripts/check-provenance.sh
scripts/check-action-pins.sh
python3 tools/vendor_framework.py pin
git ls-files -z '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' \
  | xargs -0 clang-format --dry-run -Werror
```

## Updating an action

Actions are pinned by commit sha with the release in a trailing comment, and
`scripts/check-action-pins.sh` requires both. Do not change a pin by hand unless you have
to: Dependabot proposes the move weekly as a single grouped pull request, rewriting the
sha and the comment together, which is the only way the two cannot disagree. If you do it
by hand, resolve the sha from the tag rather than trusting the tag itself:

```
gh api repos/actions/checkout/commits/v7 --jq .sha
```

Meson wrap dependencies are not covered — Dependabot has no Meson ecosystem, so `catch2`,
`pugixml`, `drm-cxx` and `wayland-cxx-scanner` move only when someone moves them.

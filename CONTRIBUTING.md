<!-- SPDX-License-Identifier: Apache-2.0 -->
# Contributing to acl

Before anything else, read `PROVENANCE.md` and `docs/clean-room.md`. The clean-room and
license rules are hard constraints enforced in CI, not guidelines.

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

## Local pre-flight (same checks CI runs)

```
reuse lint
scripts/check-spdx.sh
scripts/check-licenses.sh
scripts/check-provenance.sh
```

<!-- SPDX-License-Identifier: Apache-2.0 -->
# PROVENANCE — global source policy

This file is the authoritative, enforced source-provenance policy for the aclcxx tree.
It derives from `plan.md` §1 and Appendix A and is checked in CI
(`scripts/check-licenses.sh`, `scripts/check-provenance.sh`, `reuse lint`). When this file
and `plan.md` disagree, fix one to match — they must not drift.

## The one rule that overrides everything

**Do not open, read, fetch, quote, or paraphrase the `the GPL-3.0 predecessor` (the predecessor)
tree, the predecessor's merge requests, or the predecessor diffs — from this date forward, no exceptions.** Prior
prior analysis notes are quarantined spec inputs only and must never be pasted into `src/`.
See `docs/clean-room.md` for the compartment model and the §1.4 posture.

Strategy: **AOSP-down, not GPL-across.** Every component has a permissively-licensed
ancestor; we take the Android API surface from its Apache-2.0 AOSP upstream, never from the predecessor.

## Reference matrix (plan.md Appendix A, §A01.6)

| Reference | Status |
|---|---|
| AOSP source (Apache/BSD dirs): `frameworks/base`, `art`, `libandroidfw`, `libziparchive`, `bionic`, `packages/services/Car` | ✅ read, vendor, modify |
| Android SDK docs, developer.android.com | ✅ |
| CTS / automotive-CTS / VTS source and results | ✅ |
| Black-box behavior of apps / real devices | ✅ |
| libhybris, Anbox (Apache-2.0) | ✅ read with per-file license check |
| androidx.car.app (Jetpack), AOSP car-samples, AIDL VHAL defs | ✅ Apache-2.0 |
| vsomeip | ✅ MPL-2.0, dynamic link only |
| Waydroid | ❌ GPL-3.0 code; ✅ published docs/blog posts (record here) |
| **predecessor's tree, predecessor MRs/diffs** | ❌ from this date forward |
| Prior prior analysis notes (G1–G13, per-file symbol counts) | ⚠️ quarantined spec inputs only; never in `src/` |
| GAS binaries / OEM-proprietary VHAL / NDA docs | ❌ |

## License policy

**Permitted** in-tree or statically linked: Apache-2.0, BSD (2/3-clause), MIT, ISC, Zlib,
BSL-1.0. **LGPL-2.1 permitted dynamic-link only** (e.g. FFmpeg). **MPL-2.0 permitted
dynamic-link only** (e.g. vsomeip). GPLv2 kernel UAPI headers are fine at the syscall
boundary (not linked). 

**Forbidden** anywhere in the tree or statically linked: **GPL-3.0, LGPL-3.0, AGPL** (and
`-or-later` variants). CI (`scripts/check-licenses.sh`) rejects these SPDX identifiers.

## Per-file discipline (plan.md §8)

- Every source file carries an `SPDX-License-Identifier` header (enforced: `check-spdx.sh`,
  `reuse lint`).
- Every `src/` file carries a `// Provenance:` comment naming its sources (sanitized specs,
  AOSP docs, CTS) and ending in `No GPL-3.0 sources.`
- Every commit is DCO-signed (`Signed-off-by:`) — see `CONTRIBUTING.md`.
- Each `third_party/<dep>/` subtree carries a `METADATA` file (origin URL, tag/SHA,
  license) and the upstream `LICENSE`.

## Contamination log

Any accidental exposure to a forbidden source is logged in `docs/clean-room.md`
(Contamination log section) with scope and mitigation (reassignment of the affected module
to an unexposed implementer). An empty log is the expected steady state.

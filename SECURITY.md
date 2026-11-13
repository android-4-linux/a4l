<!-- SPDX-License-Identifier: Apache-2.0 -->
# Security policy

## Reporting a vulnerability

Report privately through GitHub, not in a public issue:
**[Security → Report a vulnerability](https://github.com/android-4-linux/a4l/security/advisories/new)**.
That opens a private advisory visible only to the maintainers, and it is the only reporting
channel this project offers — there is no security mailing list.

This is a small project worked on in the open. Expect acknowledgment within about a week. If a
report turns out to be valid, the fix, the advisory and the credit all land together; if it turns
out not to be, you will get the reasoning rather than silence.

## What is supported

`main`. There are no tagged releases and no published binaries yet, so there is nothing older to
patch and no backport branch. A report against a commit that is no longer in `main` will be
assessed against `main`.

## Posture you should know before reporting

Two properties of the current state are **known, documented and not vulnerabilities** — they are
where the project is, stated so that nobody spends a report on them:

- **APK signatures are not verified.** `pkg/apk.cpp` parses ZIP structure only. Single-shot
  APK v2/v3 verification at open time is designed and intended, mirroring Android's install-time
  check, and is not implemented.
- **There is no sandbox.** An app's native code runs as arbitrary code in acl's single glibc
  process, with no per-app UID, SELinux domain or seccomp filter of the kind on-device Android
  provides, so it can reach ART, Mesa and the Wayland connection. Containment is an architectural
  question tracked separately.

The consequence is the thing to take away: **treat an APK you run under acl as native code running
with your own privileges, because that is what it is.** Running an untrusted APK is not yet a
supported use. `docs/ndk-scope.md` has the full reasoning for both points.

A report that acl executes untrusted code it was asked to execute is therefore expected behavior.
A report that acl can be made to execute code it was *not* asked to execute — a path traversal out
of an APK, a parser bug reachable from a crafted APK, a loader that resolves a library from outside
the app's own directories — is a vulnerability, and is wanted.

## In scope

The code this project is responsible for: `src/`, `pkg/`, `tools/`, `scripts/`, the Meson build,
and the CI workflow and its gates.

## Out of scope

Third-party code this project consumes rather than writes. Report those upstream, where they can
actually be fixed:

| Component | Report to |
| --- | --- |
| AOSP framework sources, the pinned manifest's projects | [Android](https://source.android.com/docs/security/overview/report-bugs) |
| Skia | [Skia / Chromium](https://www.chromium.org/Home/chromium-security/reporting-security-bugs/) |
| `wayland-protocols` XML | [Wayland](https://gitlab.freedesktop.org/wayland/wayland-protocols) |
| `catch2`, `pugixml`, `drm-cxx`, `wayland-cxx-scanner` | their own repositories |

A vulnerability in one of those that acl's own use makes *worse* — a bad default, a check we skip,
a privilege we hold that we should not — is in scope here, and worth saying so in the report.

## Supply chain

Stated because it is what a reporter would otherwise have to work out:

- Every third-party action is pinned to a full-length commit sha, enforced both by
  `scripts/check-action-pins.sh` in CI and by GitHub's own `sha_pinning_required` on the
  repository, so a moved upstream tag cannot change what runs.
- `main` requires all CI checks and cannot be pushed to directly or force-pushed.
- Secret scanning and push protection are on. Dependabot proposes action updates; Meson wrap
  dependencies are updated by hand.
- The AOSP tree is pinned by a manifest naming all 1145 projects at explicit shas, not by a tag.
  `PROVENANCE.md` governs what may enter this tree at all.

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

- **APK signatures are checked, and not required unless you ask.** `a4l run` verifies an APK's
  v2, v3 or v3.1 signature once, before executing any of it (`src/pkg/apk_verify.cpp`). By
  default it refuses an APK whose signature is present and fails, and runs an unsigned APK with a
  warning, because development builds are unsigned. `--verify=require` runs only an APK that
  verifies. `--signer=<sha256>` runs only an APK signed by that certificate, and
  `a4l verify <app.apk>` prints the digest to pin. Under either of those two options a4l does not
  run the file you named: it copies the APK into its own cache (`~/.cache/acl/verified/`, mode
  0700), verifies the copy, and runs the copy and the native libraries extracted beside it, so
  the bytes verified are the bytes run even if the original is replaced. In the default mode the
  check is a report on the file as it was read, and the runtime reads that path again. Not
  checked: v1 (JAR) signatures, fs-verity digests, and a key-rotation lineage. Two limits
  remain. A valid signature says who signed the code, not that the code is safe. And the cache
  is protected by file ownership, so a process already running as you can rewrite it.
- **The default has no sandbox, and the sandbox that exists is not finished.** By default an
  app's native code runs as arbitrary code in a4l's single glibc process, with no per-app UID,
  SELinux domain or seccomp filter of the kind on-device Android provides, so it can reach ART,
  Mesa and the Wayland connection.
  [`docs/adr/ADR-0009`](docs/adr/ADR-0009-containment-process-boundary.md) puts everything an
  APK supplies in a confined child process that holds no display, audio or session connection,
  and lists the five conditions (C1-C5) that gate any claim to run untrusted APKs. C1, signature
  verification, is met with the limits above. C2 and C4 are met on x86-64 for apps that draw
  through Skia: `a4l run --split` runs the app in a child process inside a bubblewrap sandbox
  (its own user, mount, pid, ipc and network namespaces; no home directory, no `/run`, no
  network) under a seccomp allowlist, and the first process keeps the display. C5 is met for
  that path: the first process validates each frame the child sends against the kernel's own
  account of the buffer, and its receive path is fuzzed. C3 is not met: apps that use native GL
  or video (Flutter, libGDX) are refused under `--split`. The sandbox has had no review
  outside the project, its system call list was recorded from five apps, and the GPU render
  node stays reachable from inside it. `--split=unconfined` and `--net=host` each remove part
  of it and say so when used.

The consequence is the thing to take away: **treat an APK you run under a4l as native code running
with your own privileges, because that is what it is.** Running an untrusted APK is not yet a
supported use, with or without `--split`. `docs/ndk-scope.md` has the full reasoning for both points.

A report that a4l executes untrusted code it was asked to execute is therefore expected behavior.
A report that a4l can be made to execute code it was *not* asked to execute — a path traversal out
of an APK, a parser bug reachable from a crafted APK, a loader that resolves a library from outside
the app's own directories — is a vulnerability, and is wanted. So is a way out of the `--split`
sandbox: a file, socket, process or device the app process reaches that ADR-0009's "C4 as built"
says it cannot, or a message from the app process that makes the first process crash, leak or
hand the compositor something ADR-0009's "C5 as built" says it refuses.

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

A vulnerability in one of those that a4l's own use makes *worse* — a bad default, a check we skip,
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

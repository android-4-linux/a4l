<!-- SPDX-License-Identifier: Apache-2.0 -->
# Architecture Decision Records

Concise records of the decisions that shape acl. These summarize and point back to
`plan.md` (§3, A01, A02) — `plan.md` remains the long-form source of truth; ADRs are the
quick-reference index and the place new decisions are recorded.

Copy `template.md` for a new ADR. Number sequentially. Never delete an ADR; supersede it
with a new one and mark the old `Status: Superseded by ADR-XXXX`.

| ADR | Title | Status |
|---|---|---|
| [0001](ADR-0001-dex-execution-host-art.md) | DEX execution: host ART, not OpenJDK+DEX translation | Accepted (gate at Phase 1 exit) |
| [0002](ADR-0002-vendored-aosp-framework.md) | Java framework: vendored AOSP subset, not hand-written stubs | Accepted |
| [0003](ADR-0003-in-process-service-hub.md) | IPC: in-process service hub; binderfs deferred | Accepted |
| [0004](ADR-0004-rendering-skia-direct.md) | Rendering: Skia-direct via wcs; hwui dropped | Accepted (supersedes plan ADR-4 spike) |
| [0005](ADR-0005-ndk-bionic-staged.md) | NDK native code: staged to Phase 4 (host bionic namespaces) | Accepted |
| [0006](ADR-0006-windowing-scaling.md) | Windowing/scaling: wcs + fractional-scale + viewport | Accepted |
| [0007](ADR-0007-cal-host-native-skia.md) | CAL template host renders native Skia; Flutter optional | Accepted |

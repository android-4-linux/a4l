<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0007: CAL template host renders native Skia; Flutter optional

- **Status:** Accepted — revises plan ADR-7 per Addendum A02.3
- **Date:** 2026-07-09
- **Provenance:** plan.md §A01.2, Addendum A02.3, RA-1, RA-5. No GPL-3.0 sources.

## Context
`androidx.car.app` (Car App Library) apps never draw Views; they serialize a template tree
(`Bundleable` graph) to a platform-provided host that renders and enforces distraction rules. We
implement the host. A01 first ordered Flutter (ivi-homescreen) as primary renderer; A02 commits
the project to native Skia and downgrades the Flutter coupling.

## Decision
The **native Skia template renderer is primary**; the ivi-homescreen/Flutter renderer is an
optional integration behind the stable `ihs_shared` ABI. In the ADR-0003 single-process model the
app↔host AIDL boundary collapses to direct calls, but the serialization layer is kept for CAL
version-skew tolerance (RA-1).

## Consequences
acl-standalone is fully functional on any wlroots/Mutter/agl compositor with zero Flutter
runtime (RA-5 downgraded to L/L). The template renderer becomes the first consumer of the Skia
text substrate, so HarfBuzz/ICU/fontconfig bring-up moves ahead of Gate G1.5b. Templates staged:
Tier 1 (List, Grid, Pane, Message, LongMessage) → Tier 2 (Tab, Search, SignIn) → Tier 3
(NavigationTemplate + surface embedding, needs the Phase 2/4 `wl_subsurface` path).

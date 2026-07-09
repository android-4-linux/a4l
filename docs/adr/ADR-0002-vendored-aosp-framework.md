<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0002: Java framework — vendored AOSP subset, not hand-written stubs

- **Status:** Accepted
- **Date:** 2026-07-09
- **Provenance:** plan.md §3 ADR-2, R-2, R-4. No GPL-3.0 sources.

## Context
The `android.*` framework can be hand-written as stubs (brittle, low-fidelity, and — if copied
from the predecessor — contaminated) or vendored from AOSP `frameworks/base` (Apache-2.0, high-fidelity,
clean ancestry).

## Decision
Vendor curated packages from `frameworks/base` (view, graphics, content, os, app, util, text,
widget) at a pinned API level (**target 33 / Tiramisu**). Replace the native underside
(`android_view_*`, `android_os_*` JNI registrations) with aclcxx C++ implementations.

## Consequences
Real AOSP internals cover most reflection/hidden-API use (R-4). Risk: subsetting pulls in
Binder/SystemServer coupling (R-2) — mitigated by the **shim-at-Manager-boundary rule**: where
a class is hopelessly entangled with SystemServer/Binder, substitute a minimal source-compatible
shim written from SDK docs, **never from the predecessor**. Budget vendored class count per package.

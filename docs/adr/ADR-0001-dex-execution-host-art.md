<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0001: DEX execution — host ART, not OpenJDK+DEX translation

- **Status:** Accepted — decision/fallback gate at Phase 1 exit (Gate G1)
- **Date:** 2026-07-09
- **Provenance:** plan.md §3 ADR-1, R-1. No GPL-3.0 sources.

## Context
Apps ship DEX. Fidelity of `invoke-custom`, hidden-API semantics, `dalvik.system.*`, and JNI
behavior matters. Two options: embed AOSP's ART (host build) or translate DEX back to JVM
bytecode and run OpenJDK.

## Decision
Embed **host ART** (AOSP, Apache-2.0). It preserves DEX/Dalvik semantics exactly. OpenJDK +
d8-reversal loses fidelity and drags in `dalvik.*` shims.

## Consequences
ART host build outside the Android build system is the single hardest infrastructure item
(R-1: timebox 4 weeks, pin an api-33 tag with known host-test targets). **Fallback gate at
Gate G1:** if host ART cannot be stood up in budget, fall back to OpenJDK+Classpath-Exception
plus a `dalvik.system` compatibility layer, accepting the fidelity loss.

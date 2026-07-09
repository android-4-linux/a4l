<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0003: IPC — in-process service hub; binderfs deferred

- **Status:** Accepted
- **Date:** 2026-07-09
- **Provenance:** plan.md §3 ADR-3, §4 Phase 3. No GPL-3.0 sources.

## Context
Android's platform services are reached over Binder. Standing up Binder (binderfs + libbinder)
early is heavy, and binderfs is frequently not enabled in distro kernels.

## Decision
**Single-process model** for v1: app + framework services in one address space. `svc/` exposes
the same Java-visible Manager APIs (ActivityManager, WindowManager, PackageManager, Clipboard,
etc.) backed by direct C++ calls — no Binder.

## Consequences
Eliminates Binder for v1 and simplifies the runtime. binderfs + libbinder becomes a Phase 5
option if multi-app isolation or real `ServiceManager` semantics are needed. Where hot, service
data planes use seqlock/SPSC patterns (per `ihs_shared` lineage). In the automotive CAL host the
app↔host AIDL boundary collapses to direct calls behind the same interfaces, but the
serialization layer is kept for version-skew tolerance (see ADR-0007, RA-1).

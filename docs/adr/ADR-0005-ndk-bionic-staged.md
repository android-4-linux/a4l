<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0005: NDK native code — staged to Phase 4 (host bionic namespaces)

- **Status:** Accepted
- **Date:** 2026-07-09
- **Provenance:** plan.md §3 ADR-5, §4 Phase 4, R-6. No GPL-3.0 sources.

## Context
Apps with `lib/` ship bionic-ABI `.so` files. Running them on a glibc host requires bionic
linker namespaces bridged to host libraries — the class of problem libhybris solves in reverse.

## Decision
**Stage it.** Phase ≤3: Java/Kotlin-only apps (reject APKs whose `lib/` matters). Phase 4:
build AOSP `bionic` + `linker` for the host, load app `.so` in a bionic namespace bridged to
acl's libandroid / EGL / Vulkan trampolines into host Mesa; `AHardwareBuffer` on dmabuf.

## Consequences
Java-only remains fully shippable without any of this, so NDK risk (R-6: TLS, ifuncs, pthread
ABI) is confined to Phase 4 and off the critical path for early milestones. libhybris (Apache-2.0)
may be consulted with a per-file license check. Gate G4: an SDL-based NDK game runs; GL/Vulkan
smoke CTS subsets pass.

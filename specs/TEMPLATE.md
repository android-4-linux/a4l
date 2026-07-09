<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spec: <subsystem>

> **Provenance header (required).**
> Compartment: specification.
> Sources consulted: <AOSP paths, SDK doc URLs, CTS suites, black-box observations, and —
> only if the author is a prior-exposed spec author under posture B — quarantined predecessor notes>.
> Sanitization gate: <reviewer / date, or "self-review, posture B">.
> This spec contains NO code excerpts, NO paraphrased code, NO file-structure mirroring.

## 1. Purpose & scope
What this subsystem does; what is explicitly out of scope for the current phase.

## 2. External interfaces
Java-visible APIs (from the AOSP/SDK surface), JNI boundary, and any native ABIs. Signatures
and semantics only.

## 3. Behavior
State machines, lifecycles, ordering guarantees, error handling, threading model (how it maps
onto the reactor discipline).

## 4. Protocols & data
Wayland protocols, wire formats, file/cache layouts, IPC (in-process hub calls) touched.

## 5. Test vectors
Concrete inputs → expected outputs; CTS modules that gate this subsystem; golden references.

## 6. Open questions / decisions
Links to relevant ADRs under `docs/adr/`.

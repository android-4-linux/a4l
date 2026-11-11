<!-- SPDX-License-Identifier: Apache-2.0 -->
# Clean-room protocol

Derived from `plan.md` §1.3–§1.4. This is the operating procedure that keeps acl
provably free of the predecessor's GPLv3 expression. `PROVENANCE.md` is the enforced policy; this file
is the process.

## Why

acl reimplements the *function* of the GPL-3.0 predecessor without incorporating
or deriving from the predecessor's GPL-3.0 source. Copyright reaches the predecessor's **expression** (its C source,
hand-written Java shims, comments, file structure), not the **ideas, interface facts, or the
Android API surface** — which we take from Apache-2.0 AOSP. The clean-room protocol makes the
independence auditable.

## Two-compartment (Chinese wall) model

1. **Specification compartment.** May read anything (including the predecessor, historically). Produces
   *functional specifications only* — behavior, interfaces, protocols, test vectors. **No code
   excerpts, no paraphrased code, no file-structure mirroring.** Every spec carries a
   provenance header listing sources consulted (`specs/TEMPLATE.md`).
2. **Sanitization gate.** Each spec is reviewed to strip expressive content before release to
   implementers.
3. **Implementation compartment.** **Never opens the predecessor's tree.** Works only from sanitized specs,
   AOSP, Android SDK docs, CTS, and black-box observation of app behavior. Produces `src/`.
4. **Provenance discipline.** SPDX headers + `// Provenance:` comments on every file; DCO on
   every commit; this repo's CI rejects forbidden license IDs and denylisted references.

## The known-contamination posture (plan.md §1.4)

Prior work involved documented reading of the predecessor's source (GTK dependency inventory, per-file
symbol counts). Under a strict protocol, the prior-exposed engineer is a **spec author, not an
implementer**, for any module whose predecessor's counterpart they studied.

- **Posture A (strongest, needs ≥2 people):** prior-exposed engineer writes specs and reviews;
  unexposed engineer(s) implement.
- **Posture B (current, pragmatic solo):** quarantine all prior predecessor notes as spec inputs; rely
  on the AOSP-down strategy (the implementation's actual ancestry is Apache-2.0 AOSP, provable
  via imports and structure), the interface-inventory (facts) nature of prior exposure, and a
  from-scratch architecture (wcs/Skia/PipeWire/C++23) sharing no structural DNA with the predecessor's
  C/GTK/GObject design.

**Current posture: B.** Upgrade to A the moment a second contributor exists. Either way, from
this date: **do not open the predecessor's tree again.**

## Contamination log

Log any accidental exposure to a forbidden source here: date, who, what was seen, scope, and
mitigation (typically: reassign the affected module to an unexposed implementer; quarantine any
notes as spec-only).

| Date | Person | Source | Scope | Mitigation |
|---|---|---|---|---|
| — | — | — | — | *(none — expected steady state)* |

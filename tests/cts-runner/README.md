# CTS-runner — conformance oracle

Per `plan.md` §5. Android CTS (Apache-2.0) is the compatibility oracle: **pass-rate is
the metric, not "looks right."** This harness curates CTS module subsets per phase and
runs them in-tree against the native core (junit executed via ART; libcore/util smoke
set at Gate G1, growing per phase).

Automotive additions (`plan.md` §A01.3): VHAL-semantics / automotive-CTS subset against
the `mock` property adapter at Gate G3.

CTS is **not** vendored in Phase 0 — pinning (platform tag ↔ automotive car tag) is
recorded in `third_party/aosp/METADATA` when the runner lands.

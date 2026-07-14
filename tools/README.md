<!-- SPDX-License-Identifier: Apache-2.0 -->
# tools/

Repository tooling. Phase 0 placeholder for the items called out in `plan.md` §4 Phase 0:

- **`fw_extract.py` — frameworks/base subsetting analysis (Gate G2.c).** Given the pinned AOSP
  source tree (`third_party/aosp/METADATA` → `android-13.0.0_r84`) and a seed set of packages/
  classes, it parses Java imports to compute the transitive in-tree class dependency closure,
  reports per-package class counts against a budget (R-2), and flags every edge that reaches the
  no-Binder/SystemServer denylist (the shim-at-Manager-boundary, ADR-0002). Exit 2 on a denylist
  edge, 1 over budget, 0 clean — usable as a CI gate before any code is vendored. Example:
  `tools/fw_extract.py --src <repo>/frameworks/base/core/java --seed android.util,android.os
  --budget 80 --pkg-budget android.os=40`. Import-based closure is a first-order approximation
  (captures cross-package edges exactly; fully-qualified denylist refs are additionally scanned
  inline). Verified offline by the `fw-extract` test against `tests/fixtures/fwsrc/`.
- **AOSP mirror pinning** — the platform pin is recorded (`third_party/aosp/METADATA`:
  `android-13.0.0_r84`, manifest sha `05bc4f23…`; `external/skia` inherits the same tag).
  Automotive car-tag pairing still `TODO`.
- **app-corpus fetch** — resolve the hash-pinned F-Droid corpus manifest into
  `tests/app-corpus/apks/` (gitignored).
- **`run-apk.sh` — run an unmodified APK on the host-ART backend (dev harness).** The interactive
  counterpart to the `tests/integration/run_*_art_test.py` tests: it assembles the framework boot
  classpath, compiles `framework/acl_host` and performs the **boot-vs-app classpath split** that
  the framework shims require (see `docs/framework-bootstrap.md` A1.1), then `acl run <apk>
  --vm=art`. Needs a host ART build (`$ACL_ART_HOME`) and the AOSP host toolchain; exits 77 (skip)
  when absent. Example: `ACL_ART_HOME=… tools/run-apk.sh app.apk --present`.

These land as they are needed; nothing here is on the critical path for the Phase 0 skeleton
build. Scripts added here follow the same SPDX + provenance + DCO rules as the rest of the tree.

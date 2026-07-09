# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-1, ADR-2, §4 Phase 1. No GPL-3.0 sources.
#
# runtime/ — the runtime spine: VM-embedding seam (ART target / JVM fallback /
# Null plumbing backend), class-loader plumbing, DEX/OAT cache, JNI registration
# tables. Consumes prebuilt AOSP `art` (host build) once R-1 lands.
module_libs += static_library(
  'aclcxx_runtime',
  files('runtime.cpp', 'vm.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : fmt_dep,   # via log/log.h
)

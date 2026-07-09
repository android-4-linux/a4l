# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-1, ADR-2, §4 Phase 1. No GPL-3.0 sources.
#
# runtime/ — host ART embedding, class-loader plumbing, DEX/OAT cache,
# JNI registration tables. Consumes prebuilt AOSP `art` (host build).
module_libs += static_library(
  'aclcxx_runtime',
  files('runtime.cpp'),
  include_directories : config_inc,
)

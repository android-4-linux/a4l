# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (runtime/ DEX path), ADR-0001. No GPL-3.0 sources.
#
# dex/ — Dalvik .dex front-end for the runtime path: DexFile parser + the
# DEX->JVM converter prototype (emits compilable JVM stub classes; instruction
# translation is the larger remaining step). A stock JVM cannot load DEX, so the
# JVM backend runs converted classes; a future ART backend consumes DEX directly.
module_libs += static_library(
  'aclcxx_dex',
  files('dexfile.cpp', 'classwriter.cpp', 'dex_to_jvm.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : fmt_dep,   # via log/log.h
)

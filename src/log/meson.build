# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §4 Phase 1 (Log -> spdlog/DLT facade). No GPL-3.0 sources.
#
# log/ — three-layer logging facade. fmt backend now; spdlog/DLT sinks later.
acl_log = static_library(
  'acl_log',
  files('log.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : fmt_dep,
)
module_libs += acl_log

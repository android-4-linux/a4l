# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §A01.2, ADR-7 (A02.3 revised: native Skia primary). No GPL-3.0 sources.
#
# hosts/cal/ — Car App Library template host. Deserializes the androidx.car.app
# Bundleable template tree and renders it via the native Skia substrate (Flutter
# renderer optional, behind ihs_shared ABI). Tier 1 templates first (List, Grid,
# Pane, Message, LongMessage). Gate G1.5b.
module_libs += static_library(
  'acl_host_cal',
  files('cal.cpp'),
  include_directories : config_inc,
)

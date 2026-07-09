# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 3, §A01.2 (pulled forward for media bridge). No GPL-3.0 sources.
#
# audio/ — AudioTrack/AAudio/OpenSL shim -> PipeWire (tonic_engine lineage).
module_libs += static_library(
  'aclcxx_audio',
  files('audio.cpp'),
  include_directories : config_inc,
)

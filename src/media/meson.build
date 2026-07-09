# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 3. No GPL-3.0 sources.
#
# media/ — MediaCodec shim -> V4L2 stateless M2M primary, FFmpeg sw fallback
# (LGPL, dynamic link only). MediaPlayer facade.
module_libs += static_library(
  'aclcxx_media',
  files('media.cpp'),
  include_directories : config_inc,
)

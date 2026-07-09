# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 1. No GPL-3.0 sources.
#
# pkg/ — APK open (libziparchive), binary-XML manifest, resources.arsc via
# libandroidfw, split-APK/ABI selection. Consumes prebuilt AOSP libandroidfw.
module_libs += static_library(
  'aclcxx_pkg',
  files('pkg.cpp'),
  include_directories : config_inc,
)

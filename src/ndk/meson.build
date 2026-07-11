# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-5, §4 Phase 4. No GPL-3.0 sources.
#
# ndk/ — libandroid.so, ANativeWindow, ALooper, AAssetManager; bionic-namespace
# loader bridging app .so into host glibc/Mesa. Staged to Phase 4; Java-only apps
# ship without it.
module_libs += static_library(
  'acl_ndk',
  files('ndk.cpp'),
  include_directories : config_inc,
)

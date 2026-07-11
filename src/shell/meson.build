# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (shell/), §4. No GPL-3.0 sources.
#
# shell/ — CLI launcher, per-app data dirs, xdg desktop integration, portals.
# Builds the `acl` executable, linking every native-core module library.
# Captured so the Tier B run-path integration test can exec the real binary.
acl_exe = executable(
  'acl',
  files('main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : module_libs,
  dependencies : [fmt_dep, jni_dep],   # fmt via log/log.h; jni links libjvm when present
  install : true,
)

# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (shell/), §4. No GPL-3.0 sources.
#
# shell/ — CLI launcher, per-app data dirs, xdg desktop integration, portals.
# Builds the `aclcxx` executable, linking every native-core module library.
executable(
  'aclcxx',
  files('main.cpp'),
  include_directories : config_inc,
  link_with : module_libs,
  install : true,
)

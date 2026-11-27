# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; XDG Settings portal color-scheme
# → Configuration.uiMode UI_MODE_NIGHT_*. No GPL-3.0 sources.
#
# svc/appearance — read the desktop's color-scheme preference (XDG Settings portal) so the
# app Configuration reports the right day/night uiMode. Without sdbus-c++ the query returns -1
# and acl keeps the light default.
appearance_lib = static_library(
  'acl_svc_appearance',
  files('appearance.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep, sdbus_dep],   # fmt via log/log.h; sdbus-c++ D-Bus client
)
module_libs += appearance_lib

# Dev tool: ask for the color scheme twice, as a4l does at app start, and print each answer
# with the time it took.
appearance_probe = executable(
  'acl-appearance-probe',
  files('appearance_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [appearance_lib, acl_log],
  dependencies : [fmt_dep, sdbus_dep],
  install : false,
)

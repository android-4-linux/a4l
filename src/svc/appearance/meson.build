# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; XDG Settings portal color-scheme
# → Configuration.uiMode UI_MODE_NIGHT_*. No GPL-3.0 sources.
#
# svc/appearance — read the desktop's color-scheme preference (XDG Settings portal) so the
# app Configuration reports the right day/night uiMode. Built only when sdbus-c++ is present;
# otherwise the query returns -1 and acl keeps the light default.
appearance_lib = static_library(
  'acl_svc_appearance',
  files('appearance.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep, sdbus_dep],   # fmt via log/log.h; sdbus-c++ D-Bus client
)
module_libs += appearance_lib

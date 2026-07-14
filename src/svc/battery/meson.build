# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; host D-Bus battery mapping
# to android.os.BatteryManager / ACTION_BATTERY_CHANGED. No GPL-3.0 sources.
#
# svc/battery — battery backend: translate the host power daemon (UPower) into the
# ACTION_BATTERY_CHANGED extras. Built only when sdbus-c++ is present (LGPL-2.1,
# dynamically linked = policy-OK); otherwise svc/battery is absent and callers fall
# back to the static healthy defaults (Null backend).
battery_lib = static_library(
  'acl_svc_battery',
  files('battery_backend.cpp', 'battery_jni.cpp'),
  include_directories : [config_inc, core_inc],
  # fmt via log/log.h; sdbus-c++ D-Bus client; jni_dep for the BatteryBridge seam.
  dependencies : [fmt_dep, sdbus_dep, jni_dep],
)
module_libs += battery_lib

# Dev tool: print the selected backend + derived battery state. Handy on desktop to
# see which daemon acl picks and verify the UPower → BatteryManager mapping.
executable(
  'acl-battery-probe',
  files('battery_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [battery_lib, acl_log],   # acl_log provides the log facade impl
  dependencies : [fmt_dep, sdbus_dep],
  install : false,
)

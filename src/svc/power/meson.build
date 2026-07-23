# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; host session/power daemon
# mapping to android.os.PowerManager / IThermalService. No GPL-3.0 sources.
#
# svc/power — power backend: translate the host session manager (systemd-logind),
# the power profile daemon (power-profiles-daemon / tuned-ppd) and the kernel
# thermal zones into android.os.PowerManager state, and back Android wake locks
# with real logind inhibitor fds. The D-Bus half is gated inside the module
# (ACL_HAVE_SDBUS): without sdbus-c++ the thermal sysfs read still works and the
# session state falls back to awake defaults (Null backend).
power_deps = [fmt_dep, jni_dep]
if have_sdbus
  power_deps += sdbus_dep
endif

power_lib = static_library(
  'acl_svc_power',
  files('power_backend.cpp', 'power_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : power_deps,
)
module_libs += power_lib

# Dev tool: print the selected backend + derived power state (interactive / power
# saver / idle / thermal). Handy to verify the logind + power-profiles + thermal
# mapping on a desktop before wiring an app to it.
executable(
  'acl-power-probe',
  files('power_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [power_lib, acl_log],   # acl_log provides the log facade impl
  dependencies : power_deps,
  install : false,
)

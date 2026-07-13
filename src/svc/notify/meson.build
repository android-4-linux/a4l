# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; Android NotificationManager
# mapped to the freedesktop Desktop Notifications spec. No GPL-3.0 sources.
#
# svc/notify — post an app's notifications to the host notification daemon
# (org.freedesktop.Notifications, session bus) and route ActionInvoked/
# NotificationClosed back. Built only when sdbus-c++ is present.
notify_lib = static_library(
  'acl_svc_notify',
  files('notify_backend.cpp', 'notify_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep, sdbus_dep, jni_dep],  # jni_dep: jni.h for the NotifyBridge seam
)
module_libs += notify_lib

# Dev tool: fire a test notification through the host daemon.
executable(
  'acl-notify-probe',
  files('notify_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [notify_lib, acl_log],
  dependencies : [fmt_dep, sdbus_dep],
  install : false,
)

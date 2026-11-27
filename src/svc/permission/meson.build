# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; Android runtime permissions
# mapped to the XDG permission store's "devices" table. No GPL-3.0 sources.
#
# svc/permission — whether the app may use a device (the microphone), read from
# the desktop's permission store (org.freedesktop.impl.portal.PermissionStore,
# session bus). The D-Bus half is gated inside the module (ACL_HAVE_SDBUS):
# without sdbus-c++ only ACL_ALLOW_DEVICES grants a device.
permission_deps = [fmt_dep, jni_dep]
if have_sdbus
  permission_deps += sdbus_dep
endif

permission_lib = static_library(
  'acl_svc_permission',
  files('permission_store.cpp', 'permission_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : permission_deps,
)
module_libs += permission_lib

# Dev tool: print what the store holds for a device and an app.
executable(
  'acl-permission-probe',
  files('permission_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [permission_lib, acl_log],
  dependencies : permission_deps,
  install : false,
)

# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; host mDNS/DNS-SD daemon
# (Avahi) mapping to android.net.nsd.NsdManager. No GPL-3.0 sources.
#
# svc/nsd — service-discovery backend: translate the host mDNS/DNS-SD daemon
# (Avahi) into android.net.nsd.NsdManager (browse / resolve / advertise). Built
# only when sdbus-c++ is present (LGPL-2.1, dynamically linked = policy-OK);
# without it the Null backend reports discovery unavailable so listeners fail
# fast instead of hanging.
nsd_lib = static_library(
  'acl_svc_nsd',
  files('nsd_backend.cpp', 'nsd_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep, sdbus_dep, jni_dep],
)
module_libs += nsd_lib

# Dev tool: browse a service type against the host daemon and print what turns up,
# so the Avahi -> NsdManager mapping can be checked without an app.
executable(
  'acl-nsd-probe',
  files('nsd_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [nsd_lib, acl_log],
  dependencies : [fmt_dep, sdbus_dep],
  install : false,
)

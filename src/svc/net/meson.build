# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; host D-Bus connectivity
# mapping to android.net.ConnectivityManager. No GPL-3.0 sources.
#
# svc/net — connectivity backend: translate a host D-Bus daemon (systemd-networkd,
# NetworkManager, or ConnMan) into AndroidNetState. Built only when sdbus-c++ is
# present (LGPL-2.1, dynamically linked = policy-OK); otherwise svc/net is absent
# and callers fall back to the offline Null behavior. The -Dnet_backend combo
# picks which backends compile in (auto = all, probe at runtime).
net_lib = static_library(
  'acl_svc_net',
  files('net_backend.cpp', 'connectivity_jni.cpp'),
  include_directories : [config_inc, core_inc],
  # fmt via log/log.h; sdbus-c++ D-Bus client; jni_dep for the ConnectivityManager
  # native seam (jni.h only — all JNI calls go through the passed-in JNIEnv*).
  dependencies : [fmt_dep, sdbus_dep, jni_dep],
)
module_libs += net_lib

# Dev tool: print the selected backend + derived ConnectivityManager state. Handy
# on device/desktop to see which daemon acl picks and verify the mapping.
executable(
  'acl-net-probe',
  files('net_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [net_lib, acl_log],   # acl_log provides the log facade impl
  dependencies : [fmt_dep, sdbus_dep],
  install : false,
)

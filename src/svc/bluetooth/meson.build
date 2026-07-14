# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; host D-Bus bluetooth mapping
# to android.bluetooth.BluetoothAdapter/BluetoothDevice. No GPL-3.0 sources.
#
# svc/bluetooth — bluetooth backend: translate the host BlueZ daemon (org.bluez
# Adapter1/Device1) into BluetoothAdapter state via a cached object model. Built only
# when sdbus-c++ is present (LGPL-2.1, dynamically linked = policy-OK); otherwise
# svc/bluetooth is absent and callers fall back to the Null backend (no adapter).
bluetooth_lib = static_library(
  'acl_svc_bluetooth',
  files('bt_backend.cpp', 'bt_jni.cpp'),
  include_directories : [config_inc, core_inc],
  # fmt via log/log.h; sdbus-c++ D-Bus client; jni_dep for the BluetoothBridge seam.
  dependencies : [fmt_dep, sdbus_dep, jni_dep],
)
module_libs += bluetooth_lib

# Dev tool: print the selected backend + derived adapter state. Handy on desktop to
# see which daemon acl picks and verify the BlueZ -> BluetoothAdapter mapping.
executable(
  'acl-bluetooth-probe',
  files('bt_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [bluetooth_lib, acl_log],   # acl_log provides the log facade impl
  dependencies : [fmt_dep, sdbus_dep],
  install : false,
)

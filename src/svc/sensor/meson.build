# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; Linux IIO sysfs ABI + Android
# android.hardware.SensorManager mapping. No GPL-3.0 sources.
#
# svc/sensor — the sensor hub behind android.hardware.SensorManager. A pluggable
# SensorSource seam (Linux IIO sysfs, iio-sensor-proxy D-Bus, synthetic) feeds the
# AclSensorManager shim. Pure sysfs + synthetic need no extra deps; the optional
# iio-sensor-proxy source (later) would use sdbus-c++ like svc/battery.
sensor_lib = static_library(
  'acl_svc_sensor',
  files('sensor.cpp', 'sensor_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep, jni_dep, dependency('threads')],
)
module_libs += sensor_lib

# Dev tool / smoke demo: enumerate sensors + stream a few samples (no JVM). Shows the IIO vs
# synthetic pick and proves the poll-thread delivery — the "one good example working".
executable(
  'acl-sensor-probe',
  files('sensor_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [sensor_lib, acl_log],
  dependencies : [fmt_dep, jni_dep, dependency('threads')],
  install : false,
)

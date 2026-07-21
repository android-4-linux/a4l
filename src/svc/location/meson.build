# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; freedesktop GeoClue2 D-Bus API +
# Android android.location.LocationManager mapping. No GPL-3.0 sources.
#
# svc/location — the location hub behind android.location.LocationManager. A pluggable
# LocationSource seam (host GeoClue2 over sdbus-c++, or a synthetic fixed point) feeds the
# AclLocationManager shim. The synthetic/null source needs no D-Bus; the GeoClue2 source is
# compiled in only when sdbus-c++ is present (#if ACL_HAVE_SDBUS in location.cpp).
location_deps = [fmt_dep, jni_dep, dependency('threads')]
if have_sdbus
  location_deps += sdbus_dep
endif

location_lib = static_library(
  'acl_svc_location',
  files('location.cpp', 'location_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : location_deps,
)
module_libs += location_lib

# Dev tool / smoke demo: start the hub and stream a few fixes (no JVM). Shows the GeoClue2 vs
# synthetic pick and proves delivery — the "one good example working".
executable(
  'acl-location-probe',
  files('location_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [location_lib, acl_log],
  dependencies : location_deps,
  install : false,
)

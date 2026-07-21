# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-3, §4 Phase 3; §A01.2 (svc/car). No GPL-3.0 sources.
#
# svc/ — in-process (Binder-less) service hub: ActivityManager, WindowManager,
# PackageManager, Clipboard, SharedPreferences, ContentResolver, notifications.
svc_sources = files('svc.cpp')

# svc/net + svc/notify host D-Bus backends (ConnectivityManager / NotificationManager)
# build when sdbus-c++ is present; each its own static lib + dev probe tool.
if have_sdbus
  subdir('net')
  subdir('notify')
  subdir('battery')
  subdir('bluetooth')
  subdir('storage')
  subdir('appearance')
endif

# svc/sensor: android.hardware.SensorManager over Linux IIO (sysfs) / synthetic. No D-Bus
# needed (an optional iio-sensor-proxy source would add sdbus later); needs JNI for the
# AclSensorManager seam.
if have_jni
  subdir('sensor')
  # svc/location: android.location.LocationManager over host GeoClue2 (sdbus-c++, when present) or
  # a synthetic fixed point. Needs JNI for the AclLocationManager seam; the GeoClue2 source is
  # gated on sdbus inside the module.
  subdir('location')
endif

# Automotive vehicle services (svc/car) compile in ONLY under the automotive
# profile, so an OFF build emits zero automotive symbols (CI nm-diff gate, A01.1).
if profile_automotive
  subdir('car')
  svc_sources += svc_car_sources
endif

module_libs += static_library(
  'acl_svc',
  svc_sources,
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep],   # via log/log.h
)

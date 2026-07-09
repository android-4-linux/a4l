# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-3, §4 Phase 3; §A01.2 (svc/car). No GPL-3.0 sources.
#
# svc/ — in-process (Binder-less) service hub: ActivityManager, WindowManager,
# PackageManager, Clipboard, SharedPreferences, ContentResolver, notifications.
svc_sources = files('svc.cpp')

# Automotive vehicle services (svc/car) compile in ONLY under the automotive
# profile, so an OFF build emits zero automotive symbols (CI nm-diff gate, A01.1).
if profile_automotive
  subdir('car')
  svc_sources += svc_car_sources
endif

module_libs += static_library(
  'aclcxx_svc',
  svc_sources,
  include_directories : config_inc,
)

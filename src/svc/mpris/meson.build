# SPDX-License-Identifier: Apache-2.0
# Provenance: original; plan.md §3, §6. No GPL-3.0 sources.
#
# svc/mpris — the app's media session as an MPRIS player on the session bus.
mpris_lib = static_library(
  'acl_svc_mpris',
  files('mpris_backend.cpp', 'mpris_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep, sdbus_dep, jni_dep],  # jni_dep: jni.h for the MprisBridge seam
)
module_libs += mpris_lib

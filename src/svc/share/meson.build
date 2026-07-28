# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; Android Intent.ACTION_SEND / chooser
# mapped to xdg-desktop-portal OpenURI. No GPL-3.0 sources.
#
# svc/share — hand a shared URI to the host via xdg-desktop-portal's OpenURI (the sandbox-correct
# "open in the desktop's handler"). Built only when sdbus-c++ + JNI are present.
share_lib = static_library(
  'acl_svc_share',
  files('share_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [fmt_dep, sdbus_dep, jni_dep],  # jni_dep: jni.h for the HostShare seam
)
module_libs += share_lib

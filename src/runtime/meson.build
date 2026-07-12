# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-1, ADR-2, §4 Phase 1. No GPL-3.0 sources.
#
# runtime/ — the runtime spine: VM-embedding seam (ART target / JVM fallback /
# Null plumbing backend), class-loader plumbing, DEX/OAT cache, JNI registration
# tables. Consumes prebuilt AOSP `art` (host build) once R-1 lands.
runtime_sources = files('runtime.cpp', 'vm.cpp')
runtime_deps = [fmt_dep]   # via log/log.h

# The two JNI-Invocation-API backends (OpenJDK fallback + host-ART target), compiled
# only when the JNI headers were found. The ART backend dlopen()s libart.so at
# runtime, so it needs libdl.
if have_jni
  runtime_sources += files('jvm_vm.cpp', 'framework_natives.cpp')
  runtime_deps += jni_dep
  runtime_deps += meson.get_compiler('cpp').find_library('dl', required : false)
endif

# Under the automotive profile the runtime drives the media bridge (Gate G1.5a),
# so it link_withs the host lib (built earlier); phone builds #if it out.
runtime_link = profile_automotive ? [host_media_lib, host_cal_lib] : []

module_libs += static_library(
  'acl_runtime',
  runtime_sources,
  include_directories : [config_inc, core_inc],
  dependencies : runtime_deps,
  link_with : runtime_link,
)

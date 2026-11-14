# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 1. No GPL-3.0 sources.
#
# pkg/ — APK (zip) reader + manifest/resources. Phase 1 uses a zlib-backed zip
# reader (apk.cpp) as a placeholder for AOSP libziparchive; resources.arsc and
# binary-XML manifest parsing move to libandroidfw when it is vendored.
module_libs += static_library(
  'acl_pkg',
  files('pkg.cpp', 'apk.cpp', 'apk_stage.cpp', 'apk_verify.cpp', 'crypto_dl.cpp', 'manifest.cpp'),
  include_directories : [config_inc, core_inc],
  # libcrypto's headers only. It is loaded at run time (crypto_dl.cpp), never linked: a NEEDED
  # entry would put OpenSSL ahead of the BoringSSL that host ART loads into the same process.
  dependencies : [zlib_dep,
                  crypto_dep.partial_dependency(compile_args : true, includes : true),
                  meson.get_compiler('cpp').find_library('dl', required : false)],
)

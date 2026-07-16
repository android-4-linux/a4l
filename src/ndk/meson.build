# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-5, §4 Phase 4. No GPL-3.0 sources.
#
# ndk/ — libandroid.so, ANativeWindow, ALooper, AAssetManager; and the in-process bionic linker
# loader (bionic_runtime) that loads app lib/<abi>/*.so into the glibc host process (Stage 0b,
# docs/ndk-scope.md). Staged to Phase 4; Java-only apps ship without it.
#
# bionic_runtime.cpp is built -fno-stack-protector: it swaps the thread pointer (%fs on x86_64)
# between glibc and bionic, and the stack canary lives at %fs:0x28 — a mid-function %fs change
# would false-fail the check. The swap is confined to this TU.
acl_ndk = static_library(
  'acl_ndk',
  files('ndk.cpp', 'bionic_runtime.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : fmt_dep,
  cpp_args : ['-fno-stack-protector'],
)
module_libs += acl_ndk

# Dev tool: load a bionic .so in-process and call a symbol, to verify the loader end to end.
# Needs a staged bionic runtime at run time (ACL_BIONIC_LINKER/_SYSROOT/_STUB); build always.
executable(
  'acl-ndk-probe',
  files('ndk_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [acl_ndk, acl_log],
  dependencies : fmt_dep,
  install : false,
)

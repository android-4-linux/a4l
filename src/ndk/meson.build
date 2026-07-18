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
# jni_dep (when a JDK is present) provides jni.h for the JNIEnv/JavaVM boundary trampolines.
ndk_deps = [fmt_dep]
if have_jni
  ndk_deps += jni_dep
endif

# Per-thread attach builds a bionic TCB by hand, which needs bionic's INTERNAL struct layout.
# Measure it from the headers of the bionic we stage rather than hand-copying structs that drift
# (a moved field would not fail a build — it would corrupt the TCB). Without a usable $AOSP the
# script still succeeds, emitting ACL_BIONIC_OFFSETS_AVAILABLE 0, and attach compiles out.
# See tools/gen-bionic-offsets.sh + docs/ndk-scope.md.
bionic_offsets_h = custom_target(
  'bionic-offsets-h',
  output : 'bionic_offsets.h',
  command : [files('../../tools/gen-bionic-offsets.sh'), '@OUTPUT@'],
  depend_files : files('../../tools/bionic_offsets_probe.cpp'),
  build_by_default : true,
)

acl_ndk = static_library(
  'acl_ndk',
  files('ndk.cpp', 'bionic_runtime.cpp', 'native_bind.cpp') + [bionic_offsets_h],
  include_directories : [config_inc, core_inc],
  dependencies : ndk_deps,
  cpp_args : ['-fno-stack-protector'],
)
module_libs += acl_ndk

# Dev tool: load a bionic .so in-process and call a symbol (or the --boundary / --jni / --realvm /
# --thread self-tests), to verify the loader end to end. Needs a staged bionic runtime at run time
# (ACL_BIONIC_LINKER/_SYSROOT/_STUB); build always. tests/integration/run_ndk_attach_test.py drives
# --thread.
# The probe's --realvm self-test calls JNI_CreateJavaVM directly, so this standalone tool
# links libjvm (jvm_link_dep). The acl binary never does — it dlopens libjvm with RTLD_LOCAL.
ndk_probe_exe = executable(
  'acl-ndk-probe',
  files('ndk_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [acl_ndk, acl_log],
  dependencies : ndk_deps + (have_jni ? [jvm_link_dep] : []),
  install : false,
)

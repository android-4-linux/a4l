# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (shell/), §4. No GPL-3.0 sources.
#
# shell/ — CLI launcher, per-app data dirs, xdg desktop integration, portals.
# Builds the `acl` executable, linking every native-core module library.
# Captured so the Tier B run-path integration test can exec the real binary.

# Bake in the staged framework boot classpath (framework/meson.build) as the default
# --framework, so `acl run <apk>` resolves android.* without an explicit path. Empty
# when no JDK/javac built the framework.
shell_cpp_args = []
if framework_default_dir != ''
  shell_cpp_args += '-DACL_DEFAULT_FRAMEWORK="' + framework_default_dir + '"'
endif
if framework_android_jar != ''
  shell_cpp_args += '-DACL_DEFAULT_ANDROID_JAR="' + framework_android_jar + '"'
endif

acl_exe = executable(
  'a4l',
  files('main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : module_libs,
  dependencies : [fmt_dep, jni_dep],   # fmt via log/log.h; jni links libjvm when present
  cpp_args : shell_cpp_args,
  # Export the GL-context donor-%fs bridging seam so the dlopen'd libacl_gles_jni's gl* wrappers +
  # GLUtils (mixed Java-EGL/native-GL apps) can reach it. Per-symbol so the rest of the executable's
  # symbols stay unexported (no broad interposition surface).
  link_args : [
    '-Wl,--export-dynamic-symbol=acl_gl_run_on_donor',
    '-Wl,--export-dynamic-symbol=acl_gl_donor_route_get',
    '-Wl,--export-dynamic-symbol=acl_gl_donor_route_set',
  ],
  install : true,
)

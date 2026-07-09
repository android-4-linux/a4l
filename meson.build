# SPDX-License-Identifier: Apache-2.0
# Provenance: original build configuration. No GPL-3.0 sources.
project(
  'aclcxx',
  'cpp',
  version : '0.0.0',
  license : 'Apache-2.0',
  meson_version : '>= 1.3.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=2',
    'werror=false',
    'default_library=static',
    'b_ndebug=if-release',
  ],
)

cxx = meson.get_compiler('cpp')

# --- Options ------------------------------------------------------------------
profile_automotive = get_option('profile_automotive')
build_tests        = get_option('tests')

conf = configuration_data()
conf.set('ACLCXX_VERSION', '"' + meson.project_version() + '"')
conf.set10('ACLCXX_PROFILE_AUTOMOTIVE', profile_automotive)

# Public config header consumed across modules.
configure_file(
  output : 'aclcxx_config.h',
  configuration : conf,
)
config_inc = include_directories('.')

# Public module headers are included as "<module>/<header>.h" off the src root.
core_inc = include_directories('src')

# --- Dependencies -------------------------------------------------------------
# Heavy vendored deps (Skia via GN, AOSP via Soong) are consumed as prebuilt
# imported artifacts and wired in per subsystem as they land — see
# third_party/*/METADATA. The AOSP APK/resource stack (libziparchive,
# libandroidfw) is not vendored yet; Phase 1 pkg/ uses a zlib-backed reader
# behind an interface until it lands.
#
# Phase 1 runtime-spine dependencies (present on a normal Linux host):
fmt_dep  = dependency('fmt')      # log facade backend (spdlog/DLT sinks come later)
zlib_dep = dependency('zlib')     # APK (zip) inflate in pkg/

# Later subsystems add their system libs here as they are implemented, e.g.:
#   wayland_client = dependency('wayland-client')
#   pipewire       = dependency('libpipewire-0.3')

# --- Source tree --------------------------------------------------------------
# Each module subdir appends its static_library to `module_libs`, which the
# shell executable links. subdir() shares this scope, so the appends propagate.
module_libs = []
subdir('src')

if profile_automotive
  subdir('hosts')
endif

if build_tests
  subdir('tests')
endif

summary({
  'automotive profile' : profile_automotive,
  'tests'              : build_tests,
  'cpp_std'            : get_option('cpp_std'),
}, section : 'aclcxx')

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

# --- Dependencies -------------------------------------------------------------
# Heavy vendored deps (Skia via GN, AOSP via Soong) are consumed as prebuilt
# imported artifacts and wired in per subsystem as they land — see
# third_party/*/METADATA and subprojects/*.wrap. Phase 0 needs none of them yet.
#
# System libraries appear here as subsystems are implemented, e.g.:
#   wayland_client = dependency('wayland-client')
#   pipewire       = dependency('libpipewire-0.3')
#   libinput       = dependency('libinput')
# Kept empty in Phase 0 so the skeleton builds on a bare toolchain.

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

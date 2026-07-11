# SPDX-License-Identifier: Apache-2.0
# Provenance: original build configuration. No GPL-3.0 sources.
project(
  'aclcxx',
  'cpp',
  'c',   # only for scanner-generated protocol glue in the wm/ test compositor
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

# --- Optional OpenJDK (JVM) backend for runtime/ (ADR-0001 fallback) ----------
# Detect a JDK (JNI headers + libjvm). Meson's built-in `jni` dependency is
# unreliable across distros, so probe java.home directly. When no JDK is present
# the Jvm backend compiles out and create_vm(Jvm) reports Unavailable.
have_jni = false
jni_dep = declare_dependency()   # empty no-op placeholder
java_home = ''
java_prog = find_program('java', required : false)
if java_prog.found()
  jprops = run_command(java_prog, '-XshowSettings:properties', '-version',
                       check : false).stderr()
  parts = jprops.split('java.home = ')
  if parts.length() > 1
    java_home = parts[1].split('\n')[0].strip()
  endif
endif
if java_home != '' and import('fs').exists(java_home / 'include' / 'jni.h')
  jvm_so_dir = java_home / 'lib' / 'server'
  jvm_lib = cxx.find_library('jvm', dirs : [jvm_so_dir], required : false)
  if jvm_lib.found()
    jni_dep = declare_dependency(
      compile_args : ['-I' + java_home / 'include',
                      '-I' + java_home / 'include' / 'linux'],
      dependencies : jvm_lib,
      link_args : ['-Wl,-rpath,' + jvm_so_dir],
    )
    have_jni = true
    message('JVM runtime backend enabled: OpenJDK at ' + java_home)
  endif
endif

# audio/ backend: PipeWire when present (the AudioTrack sink actually plays);
# otherwise audio/ builds with only the counting/null sink (headless, deterministic).
pw_dep = dependency('libpipewire-0.3', required : false)
have_pipewire = pw_dep.found()

# wm/ windowing backend: wayland-cxx-scanner (wcs) CRTP bindings over wayland-client
# (plan.md Addendum A02). wcs is consumed as a prebuilt artifact: its scanner tool
# generates C++ protocol headers and its header-only wl/ framework provides the CRTP
# proxy classes + xdg-shell tables. When wcs + wayland-client + wayland-protocols are
# all present, wm/ builds the real Wayland shm presenter; otherwise wm/ is a stub and
# the Tier-C presentation test is skipped.
have_wcs = false
wcs_scanner = disabler()
wcs_dep = declare_dependency()   # empty no-op placeholder (wl/ header include added below)
wayland_dep = dependency('wayland-client', required : false)
wayland_server_dep = dependency('wayland-server', required : false)  # test compositor only
wl_scanner_dep = dependency('wayland-scanner', required : false)
wp_dep = dependency('wayland-protocols', required : false)
fs = import('fs')
wcs_root = get_option('wcs_root')
if wcs_root == ''
  # Auto-detect a sibling checkout next to this repo.
  cand = meson.project_source_root() / '..' / 'wayland-cxx-scanner'
  if fs.exists(cand / 'include' / 'wl' / 'proxy_impl.hpp')
    wcs_root = cand
  endif
endif
if wcs_root != '' and wayland_dep.found() and wl_scanner_dep.found() and wp_dep.found()
  wcs_scanner_bin = wcs_root / 'build' / 'src' / 'wayland-cxx-scanner'
  if fs.exists(wcs_scanner_bin) and fs.exists(wcs_root / 'include' / 'wl' / 'proxy_impl.hpp')
    wcs_scanner = find_program(wcs_scanner_bin, required : false)
    if wcs_scanner.found()
      have_wcs = true
      # wcs is an external sibling checkout; pass its header-only wl/ framework
      # include dir as a -I compile arg (like jni_dep for the external JDK) — an
      # absolute out-of-tree path can't go through include_directories().
      wcs_dep = declare_dependency(compile_args : ['-I' + wcs_root / 'include'])
      wl_proto_dir = wp_dep.get_variable('pkgdatadir')
      wl_core_xml = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
      xdg_shell_xml = wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
      presentation_time_xml = wl_proto_dir / 'stable' / 'presentation-time' / 'presentation-time.xml'
      message('wm/ Wayland presenter enabled: wcs at ' + wcs_root)
    endif
  endif
endif

conf = configuration_data()
conf.set('ACLCXX_VERSION', '"' + meson.project_version() + '"')
conf.set10('ACLCXX_PROFILE_AUTOMOTIVE', profile_automotive)
conf.set10('ACLCXX_HAVE_JNI', have_jni)
conf.set10('ACLCXX_HAVE_PIPEWIRE', have_pipewire)
conf.set10('ACLCXX_HAVE_WCS', have_wcs)
# javac matched to the detected libjvm (same java_home), for the DEX->JVM
# converter. Empty when no JDK; the converter then needs an explicit path.
conf.set('ACLCXX_JAVAC',
         have_jni ? '"' + java_home / 'bin' / 'javac' + '"' : '""')

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

# --- Source tree --------------------------------------------------------------
# Each module subdir appends its static_library to `module_libs`, which the
# shell executable links. subdir() shares this scope, so the appends propagate.
module_libs = []
# The wm/ test compositor is built under src/wm (where the wcs/wayland-server
# deps live) but consumed by tests/ (which runs later); a disabler default keeps
# the variable defined when it isn't built. Tests using it are guarded to match.
close_compositor = disabler()
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

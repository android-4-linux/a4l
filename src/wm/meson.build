# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-4, ADR-6, Addendum A02, §4 Phase 2. No GPL-3.0 sources.
#
# wm/ — Window/Activity surface management over wcs (xdg-shell, wl_subsurface
# for SurfaceView, wp_viewport + wp_fractional_scale_v1, xdg-activation,
# wp_presentation). ViewRootImpl-equivalent, Choreographer, damage.
#
# When wcs + wayland-client are available (have_wcs), the real Wayland shm
# presenter is built: the scanner generates the CRTP protocol headers (core
# wayland self-contained via --emit-interface-tables; xdg-shell tables come from
# <wl/xdg_shell.hpp>, so NO --emit-interface-tables there) and wayland_presenter.cpp
# uploads a gfx::Surface into a wl_shm buffer. Otherwise wm/ is a stub.

wm_sources = [files('wm.cpp')]
wm_inc = [config_inc, core_inc]
wm_deps = []

if have_wcs
  wayland_client_hpp = custom_target(
    'wm-wayland-client-header',
    input : wl_core_xml,
    output : 'wayland_client.hpp',
    command : [wcs_scanner, '--mode=client-header', '--std=c++23',
               '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
  )
  xdg_shell_client_hpp = custom_target(
    'wm-xdg-shell-client-header',
    input : xdg_shell_xml,
    output : 'xdg_shell_client.hpp',
    command : [wcs_scanner, '--mode=client-header', '--std=c++23',
               '@INPUT@', '@OUTPUT@'],
  )
  wm_sources += [files('wayland_presenter.cpp'), wayland_client_hpp, xdg_shell_client_hpp]
  # include_directories('.') resolves to BOTH src/wm (source) and build/src/wm
  # (where the generated headers land); wcs_dep supplies the wl/ framework headers.
  wm_inc += include_directories('.')
  wm_deps += [wayland_dep, wcs_dep]
else
  wm_sources += files('present_stub.cpp')
endif

wm_lib = static_library(
  'aclcxx_wm',
  wm_sources,
  include_directories : wm_inc,
  dependencies : wm_deps,
)
module_libs += wm_lib

# Tier-C presentation check: a small client rasters a gfx::Surface and presents it
# via wm/present, then run_present_test.py runs it against a headless Weston it
# launches and tears down. Only built when the real presenter is available.
if build_tests and have_wcs
  wm_present_probe = executable(
    'wm_present_probe',
    files('present_probe_main.cpp'),
    include_directories : [config_inc, core_inc],
    link_with : [wm_lib, gfx_lib, aclcxx_log],
    dependencies : wm_deps + [fmt_dep],
  )
  py3_wm = find_program('python3', required : false)
  weston_prog = find_program('weston', required : false)
  if py3_wm.found() and weston_prog.found()
    test(
      'wm-present',
      py3_wm,
      args : [files('../../tests/integration/run_present_test.py'), wm_present_probe],
      timeout : 60,
    )
  else
    message('python3/weston not both present; skipping wm-present Tier-C test.')
  endif
endif

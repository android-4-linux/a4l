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
  # presentation-time (wp_presentation) — no framework tables for it, so emit them
  # inline (self-contained), like core wayland.
  presentation_time_client_hpp = custom_target(
    'wm-presentation-time-client-header',
    input : presentation_time_xml,
    output : 'presentation_time_client.hpp',
    command : [wcs_scanner, '--mode=client-header', '--std=c++23',
               '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
  )
  wm_sources += [files('wayland_presenter.cpp'), wayland_client_hpp,
                 xdg_shell_client_hpp, presentation_time_client_hpp]
  # include_directories('.') resolves to BOTH src/wm (source) and build/src/wm
  # (where the generated headers land); wcs_dep supplies the wl/ framework headers.
  wm_inc += include_directories('.')
  wm_deps += [wayland_dep, wcs_dep, xkbcommon_dep]
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
    # Continuous presentation: the Choreographer-style present_loop animates 30
    # frames, each driven by a wl_surface.frame tick with wp_presentation timing.
    test(
      'wm-present-loop',
      py3_wm,
      args : [files('../../tests/integration/run_present_test.py'), wm_present_probe, '30'],
      timeout : 60,
    )

    # Close path: a tiny libwayland-server test compositor walks the presenter
    # through the xdg-shell handshake and then sends xdg_toplevel.close (which
    # headless Weston can't). The presenter (run unbounded, --frames 0) must react
    # to close and exit cleanly. Needs wayland-server + wayland-scanner (for the
    # xdg-shell server glue).
    wl_scanner_prog = find_program('wayland-scanner', required : false)
    if wayland_server_dep.found() and wl_scanner_prog.found()
      xdg_server_hdr = custom_target(
        'xdg-shell-server-protocol-header',
        input : xdg_shell_xml,
        output : 'xdg-shell-server-protocol.h',
        command : [wl_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@'],
      )
      xdg_protocol_code = custom_target(
        'xdg-shell-protocol-code',
        input : xdg_shell_xml,
        output : 'xdg-shell-protocol.c',
        command : [wl_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'],
      )
      close_compositor = executable(
        'close_compositor',
        [files('../../tests/integration/close_compositor.cpp'),
         xdg_server_hdr, xdg_protocol_code],
        include_directories : include_directories('.'),  # source + build src/wm (generated hdr)
        dependencies : [wayland_server_dep, xkbcommon_dep],  # xkb: keymap for key-repeat test
        # The libwayland request-vtables are intentionally partial (only the requests
        # the client sends at these versions); the rest are null by design.
        cpp_args : ['-Wno-missing-field-initializers'],
      )
      test(
        'wm-present-close',
        py3_wm,
        args : [files('../../tests/integration/run_close_test.py'),
                wm_present_probe, close_compositor],
        timeout : 60,
      )
    endif
  else
    message('python3/weston not both present; skipping wm-present Tier-C tests.')
  endif
endif

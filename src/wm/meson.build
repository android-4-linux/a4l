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

wm_sources = [files('wm.cpp', 'present_sink.cpp')]  # present_sink: split-process external-dma-buf present seam (S2)
wm_inc = [config_inc, core_inc]
wm_deps = [fmt_dep]  # present_sink logs via log/log.h
wm_cpp_args = []

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
  # text-input-v3 (IME): the wcs wl/ime/ facade includes this by a fixed name;
  # --emit-interface-tables makes it self-contained.
  text_input_v3_client_hpp = custom_target(
    'wm-text-input-v3-client-header',
    input : text_input_v3_xml,
    output : 'text_input_v3_client.hpp',
    command : [wcs_scanner, '--mode=client-header', '--std=c++23',
               '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
  )
  # fractional-scale + viewporter (HiDPI): buffer-at-physical, viewport-to-logical.
  fractional_scale_client_hpp = custom_target(
    'wm-fractional-scale-client-header',
    input : fractional_scale_xml,
    output : 'fractional_scale_v1_client.hpp',
    command : [wcs_scanner, '--mode=client-header', '--std=c++23',
               '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
  )
  viewporter_client_hpp = custom_target(
    'wm-viewporter-client-header',
    input : viewporter_xml,
    output : 'viewporter_client.hpp',
    command : [wcs_scanner, '--mode=client-header', '--std=c++23',
               '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
  )
  wm_sources += [files('wayland_presenter.cpp'), wayland_client_hpp,
                 xdg_shell_client_hpp, presentation_time_client_hpp,
                 text_input_v3_client_hpp, fractional_scale_client_hpp,
                 viewporter_client_hpp]
  # include_directories('.') resolves to BOTH src/wm (source) and build/src/wm
  # (where the generated headers land); wcs_dep supplies the wl/ framework headers.
  wm_inc += include_directories('.')
  wm_deps += [wayland_dep, wayland_cursor_dep, wcs_dep, xkbcommon_dep]
  wm_cpp_args = ['-DWL_IME_BACKEND_TEXT_INPUT_V3']  # select the wcs IME facade backend

  # P2.4/V2b: the Ganesh-Vulkan dma-buf presenter (zwp_linux_dmabuf_v1). Built only when
  # the GPU render target is also available (have_vulkan). Needs the generated
  # linux-dmabuf client header; the DmabufFrame it presents comes from gfx (acl_gfx),
  # linked by the consumer — wm/ itself needs no Vulkan headers.
  if have_vulkan
    # No --emit-interface-tables: the wcs <wl/linux_dmabuf.hpp> framework emits the
    # zwp_linux_dmabuf/buffer_params/feedback interface tables (as it does for xdg-shell).
    linux_dmabuf_client_hpp = custom_target(
      'wm-linux-dmabuf-client-header',
      input : linux_dmabuf_xml,
      output : 'linux_dmabuf_client.hpp',
      command : [wcs_scanner, '--mode=client-header', '--std=c++23',
                 '@INPUT@', '@OUTPUT@'],
    )
    # Multi-shell (ACL_WL_SHELL): besides xdg-shell (desktop), the dmabuf presenter can
    # establish the surface role via AGL's agl_shell (set_background/panel + ready — the
    # ivi-homescreen automotive pattern), the ivi-shell ivi_application (numeric surface
    # id + configure), or zwp_fullscreen_shell_v1 (the "simple" present-a-surface path).
    # These three have no wcs framework wrapper, so --emit-interface-tables makes each
    # generated header self-contained (like presentation-time / text-input). The protocol
    # XMLs are vendored (MIT) under third_party/wayland-protocols/.
    agl_shell_client_hpp = custom_target(
      'wm-agl-shell-client-header',
      input : files('../../third_party/wayland-protocols/agl-shell.xml'),
      output : 'agl_shell_client.hpp',
      command : [wcs_scanner, '--mode=client-header', '--std=c++23',
                 '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
    )
    ivi_application_client_hpp = custom_target(
      'wm-ivi-application-client-header',
      input : files('../../third_party/wayland-protocols/ivi-application.xml'),
      output : 'ivi_application_client.hpp',
      command : [wcs_scanner, '--mode=client-header', '--std=c++23',
                 '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
    )
    fullscreen_shell_client_hpp = custom_target(
      'wm-fullscreen-shell-client-header',
      input : files('../../third_party/wayland-protocols/fullscreen-shell-unstable-v1.xml'),
      output : 'fullscreen_shell_client.hpp',
      command : [wcs_scanner, '--mode=client-header', '--std=c++23',
                 '--emit-interface-tables', '@INPUT@', '@OUTPUT@'],
    )
    wm_sources += [files('dmabuf_presenter.cpp'), linux_dmabuf_client_hpp,
                   agl_shell_client_hpp, ivi_application_client_hpp,
                   fullscreen_shell_client_hpp]
  endif
else
  wm_sources += files('present_stub.cpp')
endif

# drm-cxx KMS scanout present sink (split-process appliance path, docs/split-process-art.md S2):
# imports the art-host dma-buf and atomic-flips it straight to the display. Gated on the optional
# drm-cxx dependency; without it the DrmScanout backend stays Null (present_sink.cpp).
if have_drm_cxx
  wm_sources += files('drm_scanout_sink.cpp')
  wm_deps += [drm_cxx_dep, dependency('libdrm', required : true)]  # xf86drmMode.h atomic commit
endif

wm_lib = static_library(
  'acl_wm',
  wm_sources,
  include_directories : wm_inc,
  dependencies : wm_deps,
  cpp_args : wm_cpp_args,
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
    link_with : [wm_lib, gfx_lib, acl_log],
    dependencies : wm_deps + [fmt_dep],
  )
  # P2.4/V2b: the dma-buf present probe (Ganesh Vulkan → zwp_linux_dmabuf_v1). Links
  # acl_gfx (VkRenderTarget) which pulls in the Skia + Vulkan link args. Run manually
  # against a live compositor; the automated (test-compositor) coverage lands next.
  if have_vulkan
    wm_dmabuf_probe = executable(
      'wm_dmabuf_probe',
      files('dmabuf_probe_main.cpp'),
      include_directories : [config_inc, core_inc],
      link_with : [wm_lib, gfx_lib, acl_log],
      dependencies : wm_deps + [fmt_dep],
    )
  endif
  # GLSurfaceView Stage 1 probe: host Mesa GL (gfx::GlDevice, Wayland backend) rendering
  # into acl's toplevel wl_surface, presented by Mesa via wm::present_gl_loop. Run manually
  # against a live compositor. Links acl_gfx (GlDevice) + the GL deps.
  if have_gl
    wm_gl_probe = executable(
      'wm_gl_probe',
      files('gl_probe_main.cpp'),
      include_directories : [config_inc, core_inc],
      link_with : [wm_lib, gfx_lib, acl_log],
      dependencies : wm_deps + [fmt_dep, egl_dep, glesv2_dep, gbm_dep, wl_egl_dep, gl_libdrm_dep],
    )
  endif
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
      ti_server_hdr = custom_target(
        'text-input-v3-server-protocol-header',
        input : text_input_v3_xml,
        output : 'text-input-unstable-v3-server-protocol.h',
        command : [wl_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@'],
      )
      ti_protocol_code = custom_target(
        'text-input-v3-protocol-code',
        input : text_input_v3_xml,
        output : 'text-input-unstable-v3-protocol.c',
        command : [wl_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'],
      )
      fs_server_hdr = custom_target(
        'fractional-scale-server-protocol-header',
        input : fractional_scale_xml,
        output : 'fractional-scale-v1-server-protocol.h',
        command : [wl_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@'],
      )
      fs_protocol_code = custom_target(
        'fractional-scale-protocol-code',
        input : fractional_scale_xml,
        output : 'fractional-scale-v1-protocol.c',
        command : [wl_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'],
      )
      vp_server_hdr = custom_target(
        'viewporter-server-protocol-header',
        input : viewporter_xml,
        output : 'viewporter-server-protocol.h',
        command : [wl_scanner_prog, 'server-header', '@INPUT@', '@OUTPUT@'],
      )
      vp_protocol_code = custom_target(
        'viewporter-protocol-code',
        input : viewporter_xml,
        output : 'viewporter-protocol.c',
        command : [wl_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'],
      )
      close_compositor = executable(
        'close_compositor',
        [files('../../tests/integration/close_compositor.cpp'),
         xdg_server_hdr, xdg_protocol_code, ti_server_hdr, ti_protocol_code,
         fs_server_hdr, fs_protocol_code, vp_server_hdr, vp_protocol_code],
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
      # HiDPI: the compositor sends a 1.5x fractional scale; the presenter renders a
      # physical-size buffer and maps it to logical via wp_viewport.
      test(
        'wm-present-hidpi',
        py3_wm,
        args : [files('../../tests/integration/run_hidpi_test.py'),
                wm_present_probe, close_compositor],
        timeout : 60,
      )
      # Integer HiDPI: the compositor has no fractional scale but a wl_output.scale of
      # 2; the presenter renders a 2x physical buffer and uses wl_surface.set_buffer_scale.
      test(
        'wm-present-outputscale',
        py3_wm,
        args : [files('../../tests/integration/run_outputscale_test.py'),
                wm_present_probe, close_compositor],
        timeout : 60,
      )
      # Dynamic scale: the compositor moves the surface from 1.5x to 2x mid-run; the
      # presenter must re-create its buffers at the new physical size without a restart.
      test(
        'wm-present-dynamic',
        py3_wm,
        args : [files('../../tests/integration/run_dynamic_test.py'),
                wm_present_probe, close_compositor],
        timeout : 60,
      )
    endif
  else
    message('python3/weston not both present; skipping wm-present Tier-C tests.')
  endif
endif

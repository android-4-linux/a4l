# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 3. No GPL-3.0 sources.
#
# media/ — MediaCodec shim -> V4L2 stateless M2M primary, FFmpeg sw fallback
# (LGPL, dynamic link only). MediaPlayer facade. The zero-copy video-decode path
# ([[flutter-texture-bridge]] M5): VideoDecoder produces DRM_PRIME dma-buf frames the
# ImageReader consumer imports with no copy.
media_sources = files('media.cpp')
media_deps = []
# VideoDecoder needs the GL/dma-buf interchange (gfx::DmabufFrame) and FFmpeg headers (dlopen'd at
# runtime, RTLD_LOCAL — no link, like soundpool). Built only when both are present.
if have_gl and have_ffmpeg
  media_sources += files('video_decoder.cpp')
  media_deps += [ff_dep, gl_libdrm_dep]  # gl_libdrm_dep: drm_fourcc.h (DRM_FORMAT_MOD_*)
endif
media_lib = static_library(
  'acl_media',
  media_sources,
  include_directories : [config_inc, core_inc],
  dependencies : media_deps,
)
module_libs += media_lib

# Dev tool + zero-copy proof for the video-decode path: hardware-decode a clip (VAAPI), map each
# frame to a DRM_PRIME dma-buf, ahb_import it (no copy), and sample it via GL_TEXTURE_EXTERNAL_OES.
# Headless (surfaceless EGL on a render node). Built only when GL + FFmpeg are present.
if have_gl and have_ffmpeg
  video_decode_probe = executable(
    'acl-video-decode-probe',
    files('video_decode_probe_main.cpp'),
    include_directories : [config_inc, core_inc],
    link_with : [media_lib, gfx_lib, acl_log],
    dependencies : [fmt_dep, egl_dep, glesv2_dep, gbm_dep, gl_libdrm_dep] + ff_dep,
    install : false,
  )
endif

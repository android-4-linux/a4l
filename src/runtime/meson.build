# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, ADR-1, ADR-2, §4 Phase 1. No GPL-3.0 sources.
#
# runtime/ — the runtime spine: VM-embedding seam (ART target / JVM fallback /
# Null plumbing backend), class-loader plumbing, DEX/OAT cache, JNI registration
# tables. Consumes prebuilt AOSP `art` (host build) once R-1 lands.
runtime_sources = files('runtime.cpp', 'vm.cpp', 'split_vm.cpp')
runtime_deps = [fmt_dep]   # via log/log.h; split_vm.cpp uses acl_ipc (header-only here, linked below)

# The two JNI-Invocation-API backends (OpenJDK fallback + host-ART target), compiled
# only when the JNI headers were found. The ART backend dlopen()s libart.so at
# runtime, so it needs libdl.
if have_jni
  runtime_sources += files('jvm_vm.cpp', 'framework_natives.cpp')
  if have_gl
    # android.media.ImageReader native underside over gfx::BufferQueue (Flutter external textures).
    runtime_sources += files('image_reader_jni.cpp')
    # android.media.MediaCodec native underside over media::CodecDecoder (zero-copy video decode).
    # Compiled whenever the GL path is, and guarded internally: the real body needs CodecDecoder,
    # which exists only with FFmpeg, and the file supplies a no-op registration otherwise. It must
    # not be gated here, because jvm_vm.cpp calls register_media_codec_natives under ACL_HAVE_GL
    # alone -- leaving the file out of a GL build without FFmpeg takes both definitions with it.
    runtime_sources += files('media_codec_jni.cpp')
    # MediaCodecList: advertise acl's HW decoder to ExoPlayer's codec discovery (no FFmpeg dep).
    runtime_sources += files('media_codec_list_jni.cpp')
  endif
  runtime_deps += jni_dep
  runtime_deps += meson.get_compiler('cpp').find_library('dl', required : false)
  # GLSurfaceView: jvm_vm.cpp registers the EGLImpl natives (gfx/egl10_jni.h) + drives
  # gfx::GlDevice, so it needs the EGL headers (the symbols come from gfx_lib at final link).
  if have_gl
    runtime_deps += egl_dep
  endif
endif

# Under the automotive profile the runtime drives the media bridge (Gate G1.5a),
# so it link_withs the host lib (built earlier); phone builds #if it out.
runtime_link = profile_automotive ? [host_media_lib, host_cal_lib] : []

module_libs += static_library(
  'acl_runtime',
  runtime_sources,
  include_directories : [config_inc, core_inc],
  dependencies : runtime_deps,
  link_with : runtime_link,
)

# acl-art-host (split-process ART, docs/split-process-art.md S1): the render/VM side that boots a
# runtime via the JNI Invocation API and forwards its output over acl_ipc. Built here as a glibc
# binary for x86 dev/CI (drives OpenJDK); the identical source rebuilds linux_bionic-arm64 in the
# AOSP tree for the Pi (stock bionic host ART). Needs jni.h + libdl + threads + acl_ipc.
if have_jni
  threads_dep = dependency('threads')
  art_host_exe = executable(
    'acl-art-host',
    files('art_host_main.cpp'),
    include_directories : [config_inc, core_inc],
    dependencies : [jni_dep, threads_dep,
                    meson.get_compiler('cpp').find_library('dl', required : false)],
    link_with : [ipc_lib],
    install : false,
  )
endif

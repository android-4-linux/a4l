# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 3, §A01.2 (pulled forward for media bridge). No GPL-3.0 sources.
#
# audio/ — the AudioTrack sink: a counting/null backend (always) plus a PipeWire
# backend that streams to the running daemon (built when libpipewire is present).
audio_sources = files('audio.cpp')
audio_deps = [fmt_dep]   # via log/log.h

if have_pipewire
  # PipeWire AudioTrack sink + the master-volume controller (default-sink tracking) and its
  # JNI seam backing org.acl.internal.AudioBridge → android.media.AudioManager.
  audio_sources += files('pipewire_sink.cpp', 'pw_volume.cpp', 'audio_jni.cpp')
  audio_deps += [pw_dep, jni_dep]  # jni_dep = jni.h only (calls go through the passed JNIEnv*)
endif

audio_lib = static_library(
  'acl_audio',
  audio_sources,
  include_directories : [config_inc, core_inc],
  dependencies : audio_deps,
)
module_libs += audio_lib

# Dev tool: print / set the host PipeWire master-output volume (the default sink), to verify the
# svc/audio → AudioManager mapping. Built only when PipeWire is present.
if have_pipewire
  executable(
    'acl-audio-probe',
    files('pw_volume_probe_main.cpp'),
    include_directories : [config_inc, core_inc],
    link_with : [audio_lib, acl_log],
    dependencies : [fmt_dep, pw_dep],
    install : false,
  )
endif

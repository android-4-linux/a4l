# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 3, §A01.2 (pulled forward for media bridge). No GPL-3.0 sources.
#
# audio/ — the AudioTrack sink: a counting/null backend (always) plus a PipeWire
# backend that streams to the running daemon (built when libpipewire is present).
audio_sources = files('audio.cpp')
audio_deps = [fmt_dep]   # via log/log.h

if have_pipewire
  audio_sources += files('pipewire_sink.cpp')
  audio_deps += pw_dep
endif

module_libs += static_library(
  'aclcxx_audio',
  audio_sources,
  include_directories : [config_inc, core_inc],
  dependencies : audio_deps,
)

# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l design (ADR-0009 Decision 2, C4). No GPL-3.0 sources.
#
# confine/ - confinement of the --split app process: the bubblewrap launch
# plan (namespaces, the filesystem view) and the seccomp-bpf allowlist the app
# process installs on itself. Kernel and bubblewrap interfaces only; links
# nothing.
confine_lib = static_library(
  'acl_confine',
  files('confine.cpp', 'seccomp_filter.cpp'),
  include_directories : [config_inc, core_inc],
)
module_libs += confine_lib

confine_dep = declare_dependency(
  link_with : confine_lib,
  include_directories : [config_inc, core_inc],
)

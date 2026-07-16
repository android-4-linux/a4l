# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl design (docs/split-process-art.md). No GPL-3.0 sources.
#
# ipc/ — the acl_ipc transport between the bionic art-host process and the glibc acl process
# (SOCK_SEQPACKET + SCM_RIGHTS dma-buf/shm fd passing). Standard POSIX only, so the identical source
# also compiles into the bionic art-host executable (built out-of-tree, linux_bionic-arm64).
ipc_lib = static_library(
  'acl_ipc',
  files('ipc.cpp'),
  include_directories : [config_inc, core_inc],
)
module_libs += ipc_lib

ipc_dep = declare_dependency(
  link_with : ipc_lib,
  include_directories : [config_inc, core_inc],
)

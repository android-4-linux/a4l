// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md, S1). No GPL-3.0 sources.
#ifndef ACL_RUNTIME_SPLIT_VM_H
#define ACL_RUNTIME_SPLIT_VM_H

#include <memory>

#include "runtime/vm.h"

namespace acl::runtime {

// Backend::ArtSplit — the arm64 split-process path. Instead of embedding ART in-process (impossible
// on arm64: a glibc acl can't dlopen the bionic libart.so, docs/pi-plan.md §P-B/M3), acl launches the
// bionic `acl-art-host` executable, which boots the VM and runs the app, and drives it over acl_ipc.
// On x86 this drives an OpenJDK libjvm.so (dev/CI); on the Pi, the stock bionic host ART's libart.so.
//
// Env seams: ACL_ART_HOST (path to the acl-art-host executable; default: alongside the acl binary),
// ACL_ART_SPLIT_VMLIB (the runtime library art-host dlopens; default: $ACL_ART_HOME/lib64/libart.so).
std::unique_ptr<Vm> make_split_vm(VmConfig config);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_SPLIT_VM_H

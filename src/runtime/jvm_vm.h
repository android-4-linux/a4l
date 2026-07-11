// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/runtime.md (pending), JNI Invocation API docs, ADR-0001. No GPL-3.0 sources.
#ifndef ACL_RUNTIME_JVM_VM_H
#define ACL_RUNTIME_JVM_VM_H

// Factory for the OpenJDK-backed VM (ADR-0001 fallback). Declared separately so
// jni.h stays confined to jvm_vm.cpp and out of the public runtime seam. Only
// meaningful when the build detected a JDK (ACL_HAVE_JNI); create_vm() calls
// this under that guard.

#include <memory>

#include "runtime/vm.h"

namespace acl::runtime {

std::unique_ptr<Vm> make_jvm_vm(VmConfig config);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_JVM_VM_H

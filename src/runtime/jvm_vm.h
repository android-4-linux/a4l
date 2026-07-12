// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/runtime.md (pending), JNI Invocation API docs, ADR-0001. No GPL-3.0 sources.
#ifndef ACL_RUNTIME_JVM_VM_H
#define ACL_RUNTIME_JVM_VM_H

// Factories for the two JNI-Invocation-API backends. Declared separately so jni.h
// stays confined to jvm_vm.cpp and out of the public runtime seam. Only meaningful
// when the build detected the JNI headers (ACL_HAVE_JNI); create_vm() calls these
// under that guard.
//
// make_jvm_vm: OpenJDK/HotSpot (ADR-0001 fallback), linked at build time.
// make_art_vm: host ART (ADR-0001 target), dlopen'd at runtime from ACL_ART_HOME —
//   runs the app's DEX natively (no translation).

#include <memory>

#include "runtime/vm.h"

namespace acl::runtime {

std::unique_ptr<Vm> make_jvm_vm(VmConfig config);
std::unique_ptr<Vm> make_art_vm(VmConfig config);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_JVM_VM_H

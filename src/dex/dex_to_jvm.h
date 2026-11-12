// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), Dalvik bytecode & JVM Specification,
// ADR-0001. No GPL-3.0 sources.
#ifndef ACL_DEX_DEX_TO_JVM_H
#define ACL_DEX_DEX_TO_JVM_H

// DEX -> JVM converter. A stock JVM cannot load Dalvik bytecode, so the
// ADR-0001 JVM backend runs converted classes. This emits real .class files (no
// javac): a small, linear integer subset of Dalvik bytecode
// (const/move/int-arithmetic/ return, static methods) is TRANSLATED to
// executing JVM bytecode; any method using an unsupported opcode/type falls
// back to a bytecode stub whose body throws UnsupportedOperationException.
// Branch/invoke/field/wide/exception handling is the remaining translation
// surface and plugs in behind translate().

#include <string>
#include <vector>

#include "dex/dexfile.h"

namespace acl::dex {

struct ConvertOptions {
  std::string work_dir;  // output scratch dir; a temp dir is used if empty
};

struct ConvertResult {
  std::string classpath_dir;         // directory of emitted .class files
  std::vector<std::string> classes;  // Java names emitted
  int translated_methods = 0;        // methods with real translated bytecode
  int stubbed_methods = 0;           // methods left as throwing stubs
};

// Emit .class files for `dex`'s classes into a directory for the JVM classpath.
DexError convert(const DexFile& dex,
                 const ConvertOptions& opts,
                 ConvertResult& out);

}  // namespace acl::dex

#endif  // ACL_DEX_DEX_TO_JVM_H

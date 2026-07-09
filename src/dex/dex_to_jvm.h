// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), JVM class semantics, ADR-0001. No GPL-3.0 sources.
#ifndef ACLCXX_DEX_DEX_TO_JVM_H
#define ACLCXX_DEX_DEX_TO_JVM_H

// DEX -> JVM converter (PROTOTYPE). A stock JVM cannot load Dalvik bytecode, so
// the ADR-0001 JVM backend runs converted classes. This prototype reproduces the
// class/method SHAPE (names, superclass-as-Object, signatures) as compilable Java
// stubs whose bodies throw UnsupportedOperationException, then compiles them with
// javac. It proves the pipeline end to end — DEX parse -> emit -> compile -> JVM
// load & method resolution — but does NOT translate method bytecode. Real Dalvik
// ->JVM instruction translation (what dex2jar/enjarify do) is the larger next
// step and plugs in behind emit_java_stub()/convert().

#include <string>
#include <vector>

#include "dex/dexfile.h"

namespace aclcxx::dex {

// Emit compilable Java source for one class's shape (pure; no I/O). Skips
// interfaces/annotations and non-translatable members (<init>, <clinit>,
// abstract, native). Method bodies throw UnsupportedOperationException.
std::string emit_java_stub(const DexClass& cls);

struct ConvertOptions {
  std::string javac_path;  // defaults to the build-detected ACLCXX_JAVAC
  std::string work_dir;    // scratch dir; a temp dir is created if empty
};

struct ConvertResult {
  std::string classpath_dir;         // directory of compiled .class files
  std::vector<std::string> classes;  // Java names emitted
};

// Full pipeline: write Java stubs for `dex`'s classes and compile them with
// javac into a directory suitable for the JVM classpath. Requires a javac.
DexError convert(const DexFile& dex, const ConvertOptions& opts,
                 ConvertResult& out);

}  // namespace aclcxx::dex

#endif  // ACLCXX_DEX_DEX_TO_JVM_H

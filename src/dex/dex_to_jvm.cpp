// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), JVM class semantics, ADR-0001. No GPL-3.0 sources.

#include "dex/dex_to_jvm.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "aclcxx_config.h"
#include "log/log.h"

namespace fs = std::filesystem;

namespace aclcxx::dex {
namespace {

constexpr std::string_view kTag = "aclcxx.dex";

// ACC_INTERFACE / ACC_ANNOTATION / ACC_ENUM at class level.
constexpr std::uint32_t kAccInterface = 0x0200;
constexpr std::uint32_t kAccAnnotation = 0x2000;

bool skippable_class(const DexClass& c) {
  return (c.access_flags & (kAccInterface | kAccAnnotation)) != 0;
}

bool emittable_method(const DexMethod& m) {
  if (m.name == "<init>" || m.name == "<clinit>") return false;
  if (m.access_flags & (kAccAbstract | kAccNative)) return false;
  // A valid Java identifier is required for the source stub.
  if (m.name.empty()) return false;
  for (char c : m.name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) {
      return false;
    }
  }
  return true;
}

std::pair<std::string, std::string> split_package(const std::string& java_name) {
  const std::size_t dot = java_name.rfind('.');
  if (dot == std::string::npos) return {"", java_name};
  return {java_name.substr(0, dot), java_name.substr(dot + 1)};
}

// Run argv (no shell). Returns exit code, or -1 on spawn failure.
int run_process(const std::vector<std::string>& argv) {
  std::vector<char*> c;
  c.reserve(argv.size() + 1);
  for (const std::string& a : argv) c.push_back(const_cast<char*>(a.c_str()));
  c.push_back(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    ::execvp(c[0], c.data());
    _exit(127);
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

std::string emit_java_stub(const DexClass& cls) {
  const auto [pkg, simple] = split_package(cls.java_name);
  std::string out;
  if (!pkg.empty()) out += "package " + pkg + ";\n\n";
  // Superclass is flattened to Object: the prototype does not reconstruct the
  // app class hierarchy (that requires all classes + their constructors).
  out += "public class " + simple + " {\n";

  auto emit = [&](const DexMethod& m) {
    if (!emittable_method(m)) return;
    out += "  public ";
    if (m.is_static()) out += "static ";
    out += m.return_type + " " + m.name + "(";
    for (std::size_t i = 0; i < m.param_types.size(); ++i) {
      if (i) out += ", ";
      out += m.param_types[i] + " a" + std::to_string(i);
    }
    out += ") {\n";
    out += "    throw new UnsupportedOperationException(\"aclcxx: DEX method not "
           "translated: " + simple + "." + m.name + "\");\n";
    out += "  }\n";
  };
  for (const DexMethod& m : cls.direct_methods) emit(m);
  for (const DexMethod& m : cls.virtual_methods) emit(m);

  out += "}\n";
  return out;
}

DexError convert(const DexFile& dex, const ConvertOptions& opts,
                 ConvertResult& out) {
  std::string javac = opts.javac_path.empty() ? std::string(ACLCXX_JAVAC)
                                              : opts.javac_path;
  if (javac.empty()) {
    return {DexStatus::Unsupported,
            "no javac available (build without a JDK); set ConvertOptions.javac_path"};
  }

  std::error_code ec;
  fs::path work = opts.work_dir.empty()
                      ? fs::temp_directory_path(ec) / "aclcxx-dex2jvm"
                      : fs::path(opts.work_dir);
  const fs::path src = work / "src";
  const fs::path classes = work / "classes";
  fs::create_directories(src, ec);
  fs::create_directories(classes, ec);
  if (ec) return {DexStatus::Unsupported, "cannot create work dir: " + ec.message()};

  std::vector<std::string> java_files;
  for (const DexClass& cls : dex.classes()) {
    if (skippable_class(cls)) continue;
    const auto [pkg, simple] = split_package(cls.java_name);
    fs::path dir = src;
    if (!pkg.empty()) {
      std::string rel = pkg;
      for (char& ch : rel) {
        if (ch == '.') ch = '/';
      }
      dir /= rel;
      fs::create_directories(dir, ec);
    }
    const fs::path file = dir / (simple + ".java");
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return {DexStatus::Unsupported, "cannot write " + file.string()};
    f << emit_java_stub(cls);
    f.close();
    java_files.push_back(file.string());
    out.classes.push_back(cls.java_name);
  }

  if (java_files.empty()) {
    return {DexStatus::Unsupported, "no emittable classes in dex"};
  }

  std::vector<std::string> argv = {javac, "-d", classes.string()};
  argv.insert(argv.end(), java_files.begin(), java_files.end());
  const int rc = run_process(argv);
  if (rc != 0) {
    return {DexStatus::Unsupported,
            "javac failed (rc=" + std::to_string(rc) + ") using " + javac};
  }

  out.classpath_dir = classes.string();
  log::i(kTag, "converted {} class(es) -> {}", out.classes.size(),
         out.classpath_dir);
  return {};
}

}  // namespace aclcxx::dex

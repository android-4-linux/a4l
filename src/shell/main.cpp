// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/shell.md (pending), Android SDK launch semantics, xdg-desktop specs. No GPL-3.0 sources.
//
// aclcxx CLI. `aclcxx run <app.apk>` drives the Phase 1 runtime-spine vertical:
// open the APK (pkg), inspect its DEX/ABIs, configure a VM, and invoke a static
// main() through the runtime seam. The ART/JVM backends report Unavailable until
// they land (R-1 / ADR-0001); `--vm=null` exercises the full plumbing today.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>  // getpid, for a per-process DEX conversion work dir

#include "aclcxx_config.h"
#include "dex/dex_to_jvm.h"
#include "dex/dexfile.h"
#include "log/log.h"
#include "pkg/apk.h"
#include "pkg/manifest.h"
#include "runtime/vm.h"

namespace {

constexpr std::string_view kTag = "aclcxx";

// Exit codes.
enum : int { kOk = 0, kUsage = 1, kBackendUnavailable = 2, kRuntimeError = 3 };

void print_version() { std::printf("aclcxx %s\n", ACLCXX_VERSION); }

void print_roster() {
  // Phase 0 smoke: prove the native-core module link graph resolves.
  namespace r = aclcxx::runtime;
  std::printf("profile: %s\n",
              ACLCXX_PROFILE_AUTOMOTIVE ? "automotive" : "phone");
  std::printf("runtime backends: art (target), jvm (fallback), null (plumbing)\n");
  (void)r::backend_name(r::Backend::Art);
}

void print_usage() {
  std::printf(
      "usage: aclcxx <command> [options]\n"
      "\n"
      "commands:\n"
      "  run <app.apk>        inspect and launch an APK\n"
      "  dex <file>           dump classes/methods of a .dex or .apk\n"
      "  (no command)         print version and build profile\n"
      "\n"
      "run options:\n"
      "  --vm=<art|jvm|null>  runtime backend (default: $ACLCXX_VM or art)\n"
      "  --main=<class>       static main() class to invoke\n"
      "  --classpath=<paths>  extra classpath entries (colon-separated); the JVM\n"
      "                       backend runs .class/.jar from here (DEX is not loaded\n"
      "                       by a stock JVM)\n"
      "  --convert-dex        convert the APK's DEX to JVM classes and add them to\n"
      "                       the classpath (implied by --vm=jvm)\n"
      "  --framework=<paths>  vendored android.* framework boot classpath (colon-\n"
      "                       separated); also read from $ACLCXX_FRAMEWORK\n"
      "  --cache=<dir>        DEX->OAT / dalvik-cache directory\n"
      "\n"
      "  -h, --help           show this help\n"
      "  -v, --version        show version\n");
}

// Split "--key=value"; returns true and fills value when arg starts with key.
bool match_opt(std::string_view arg, std::string_view key, std::string& value) {
  if (arg.substr(0, key.size()) == key && arg.size() > key.size() &&
      arg[key.size()] == '=') {
    value = std::string(arg.substr(key.size() + 1));
    return true;
  }
  return false;
}

int cmd_run(const std::vector<std::string>& args) {
  namespace r = aclcxx::runtime;

  std::string apk_path;
  std::string vm_name;
  std::string main_class;
  std::string cache_dir;
  std::vector<std::string> extra_classpath;
  std::vector<std::string> framework_dirs;
  bool convert_dex = false;

  // Split a colon-separated path list into `out`, skipping empties.
  const auto split_paths = [](const std::string& v, std::vector<std::string>& out) {
    for (std::size_t start = 0; start <= v.size();) {
      const std::size_t colon = v.find(':', start);
      const std::size_t end = colon == std::string::npos ? v.size() : colon;
      if (end > start) out.push_back(v.substr(start, end - start));
      if (colon == std::string::npos) break;
      start = colon + 1;
    }
  };

  for (const std::string& a : args) {
    std::string v;
    if (a == "--convert-dex") {
      convert_dex = true;
    } else if (match_opt(a, "--vm", v)) {
      vm_name = v;
    } else if (match_opt(a, "--framework", v)) {
      split_paths(v, framework_dirs);  // vendored android.* boot classpath
    } else if (match_opt(a, "--main", v)) {
      main_class = v;
    } else if (match_opt(a, "--classpath", v)) {
      split_paths(v, extra_classpath);  // colon-separated, appended in order
    } else if (match_opt(a, "--cache", v)) {
      cache_dir = v;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "run: unknown option '%s'\n", a.c_str());
      return kUsage;
    } else if (apk_path.empty()) {
      apk_path = a;
    } else {
      std::fprintf(stderr, "run: unexpected argument '%s'\n", a.c_str());
      return kUsage;
    }
  }
  if (apk_path.empty()) {
    std::fprintf(stderr, "run: missing <app.apk>\n");
    return kUsage;
  }

  // Select the backend.
  r::Backend backend = r::default_backend();
  if (!vm_name.empty() && !r::parse_backend(vm_name, backend)) {
    std::fprintf(stderr, "run: unknown --vm '%s' (art|jvm|null)\n", vm_name.c_str());
    return kUsage;
  }

  // Open and inspect the APK.
  aclcxx::pkg::ZipError zerr;
  auto apk = aclcxx::pkg::Apk::open(apk_path, zerr);
  if (!apk.has_value()) {
    aclcxx::log::e(kTag, "cannot open {}: [{}] {}", apk_path,
                   aclcxx::pkg::zip_status_name(zerr.status), zerr.message);
    return kUsage;
  }

  std::vector<std::string> dex_files;
  for (const auto& e : apk->entries()) {
    if (e.name.rfind("classes", 0) == 0 && e.name.size() > 4 &&
        e.name.compare(e.name.size() - 4, 4, ".dex") == 0 &&
        e.name.find('/') == std::string::npos) {
      dex_files.push_back(e.name);
    }
  }
  const std::vector<std::string> abis = apk->native_abis();

  aclcxx::log::i(kTag, "opened {} ({} entries, {} dex, abis=[{}])", apk_path,
                 apk->entries().size(), dex_files.size(),
                 [&] {
                   std::string s;
                   for (std::size_t i = 0; i < abis.size(); ++i) {
                     if (i) s += ',';
                     s += abis[i];
                   }
                   return s;
                 }());
  if (dex_files.empty()) {
    aclcxx::log::w(kTag, "no classes*.dex found — nothing to execute");
  }

  // Parse the manifest for package identity and the launch class.
  aclcxx::pkg::Manifest manifest;
  aclcxx::pkg::ZipError merr;
  if (auto mbytes = apk->read("AndroidManifest.xml", merr)) {
    if (aclcxx::pkg::AxmlError ae = aclcxx::pkg::parse_manifest(*mbytes, manifest)) {
      aclcxx::log::w(kTag, "manifest parse failed: [{}] {}",
                     aclcxx::pkg::axml_status_name(ae.status), ae.message);
    } else {
      aclcxx::log::i(kTag, "package={} versionCode={} minSdk={} targetSdk={}",
                     manifest.package.empty() ? "?" : manifest.package,
                     manifest.version_code ? *manifest.version_code : 0,
                     manifest.min_sdk ? *manifest.min_sdk : -1,
                     manifest.target_sdk ? *manifest.target_sdk : -1);
    }
  } else {
    aclcxx::log::w(kTag, "no AndroidManifest.xml: [{}] {}",
                   aclcxx::pkg::zip_status_name(merr.status), merr.message);
  }

  // Resolve the launch class: explicit --main wins, else the manifest launcher.
  if (main_class.empty()) {
    if (const aclcxx::pkg::Activity* launch = manifest.launcher()) {
      main_class = launch->name;
      aclcxx::log::i(kTag, "launch activity: {}", main_class);
    }
  }

  // Configure the runtime. The APK is passed on the classpath; extraction to a
  // real DEX/OAT cache is a later step (needs the framework boot classpath).
  r::VmConfig cfg;
  cfg.classpath = {apk_path};
  cfg.classpath.insert(cfg.classpath.end(), extra_classpath.begin(),
                       extra_classpath.end());
  cfg.cache_dir = cache_dir;

  // Boot classpath = the vendored android.* framework (Gate G2). Sources: --framework
  // then $ACLCXX_FRAMEWORK. Empty today (no framework vendored yet); the seam is live.
  cfg.boot_classpath = framework_dirs;
  if (const char* env = std::getenv("ACLCXX_FRAMEWORK")) {
    split_paths(env, cfg.boot_classpath);
  }
  if (!cfg.boot_classpath.empty()) {
    aclcxx::log::i(kTag, "framework boot classpath: {} entr{}",
                   cfg.boot_classpath.size(),
                   cfg.boot_classpath.size() == 1 ? "y" : "ies");
  }

  // DEX->JVM: convert each classes*.dex to JVM classes and add them to the
  // classpath so the backend can load DEX-derived classes. A stock JVM cannot
  // load DEX at all, so conversion is implied for the jvm backend.
  const bool do_convert = convert_dex || backend == r::Backend::Jvm;
  if (do_convert && !dex_files.empty()) {
    int dex_index = 0;
    for (const std::string& dex_name : dex_files) {
      const int idx = dex_index++;
      auto db = apk->read(dex_name, zerr);
      if (!db) {
        aclcxx::log::w(kTag, "convert-dex: cannot read {}", dex_name);
        continue;
      }
      aclcxx::dex::DexError de;
      auto dx = aclcxx::dex::DexFile::parse(*db, de);
      if (!dx) {
        aclcxx::log::w(kTag, "convert-dex: parse {} failed: {}", dex_name, de.message);
        continue;
      }
      // A work dir unique per process AND per DEX, so concurrent `aclcxx run`
      // invocations (and multi-dex within one) never clobber each other's classes.
      aclcxx::dex::ConvertOptions opts;
      opts.work_dir =
          (std::filesystem::temp_directory_path() /
           ("aclcxx-" + std::to_string(::getpid()) + "-dex-" + std::to_string(idx)))
              .string();
      aclcxx::dex::ConvertResult cr;
      if (aclcxx::dex::DexError ce = aclcxx::dex::convert(*dx, opts, cr)) {
        aclcxx::log::w(kTag, "convert-dex: {} failed: {}", dex_name, ce.message);
        continue;
      }
      aclcxx::log::i(kTag, "convert-dex: {} -> {} ({} classes)", dex_name,
                     cr.classpath_dir, cr.classes.size());
      cfg.classpath.push_back(cr.classpath_dir);
    }
  }

  r::Error err;
  auto vm = r::create_vm(backend, cfg, err);
  if (vm == nullptr) {
    aclcxx::log::e(kTag, "runtime backend '{}' unavailable: {}",
                   r::backend_name(backend), err.message);
    return kBackendUnavailable;
  }

  if (r::Error e = vm->start()) {
    aclcxx::log::e(kTag, "vm start failed: [{}] {}", r::status_name(e.status),
                   e.message);
    return kRuntimeError;
  }

  if (main_class.empty()) {
    aclcxx::log::w(kTag,
                   "no launcher activity in the manifest and no --main given; "
                   "VM started, nothing invoked");
  } else {
    // Prefer the Android Activity lifecycle; fall back to a static main() for
    // self-contained apps (or when no framework Activity is on the boot classpath).
    r::Error e = vm->call_activity_lifecycle(main_class);
    if (e.status == r::Status::NotApplicable) {
      aclcxx::log::i(kTag, "{} is not an Activity; invoking static main()", main_class);
      e = vm->call_static_main(main_class, {});
    }
    if (e) {
      aclcxx::log::e(kTag, "launch {} failed: [{}] {}", main_class,
                     r::status_name(e.status), e.message);
      vm->shutdown();
      return kRuntimeError;
    }
  }

  vm->shutdown();
  return kOk;
}

void dump_methods(const char* kind, const std::vector<aclcxx::dex::DexMethod>& ms) {
  for (const aclcxx::dex::DexMethod& m : ms) {
    std::string params;
    for (std::size_t i = 0; i < m.param_types.size(); ++i) {
      if (i) params += ", ";
      params += m.param_types[i];
    }
    std::printf("    %s%s %s(%s) [%s%s]\n", m.is_static() ? "static " : "",
                m.return_type.c_str(), m.name.c_str(), params.c_str(), kind,
                m.has_code() ? ", code" : "");
  }
}

int cmd_dex(const std::vector<std::string>& args) {
  if (args.size() != 1) {
    std::fprintf(stderr, "dex: expected exactly one <file>\n");
    return kUsage;
  }
  const std::string& path = args[0];

  // Accept a raw .dex or an .apk (from which classes.dex is extracted).
  std::vector<std::uint8_t> bytes;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "dex: cannot open %s\n", path.c_str());
    return kUsage;
  }
  bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  const bool is_dex = bytes.size() >= 4 && std::memcmp(bytes.data(), "dex\n", 4) == 0;
  if (!is_dex) {
    aclcxx::pkg::ZipError zerr;
    auto apk = aclcxx::pkg::Apk::open(path, zerr);
    if (!apk.has_value()) {
      std::fprintf(stderr, "dex: %s is neither a dex nor a readable apk: %s\n",
                   path.c_str(), zerr.message.c_str());
      return kUsage;
    }
    auto db = apk->read("classes.dex", zerr);
    if (!db) {
      std::fprintf(stderr, "dex: no classes.dex in %s\n", path.c_str());
      return kUsage;
    }
    bytes = std::move(*db);
  }

  aclcxx::dex::DexError err;
  auto dex = aclcxx::dex::DexFile::parse(bytes, err);
  if (!dex.has_value()) {
    std::fprintf(stderr, "dex: parse failed: [%s] %s\n",
                 aclcxx::dex::dex_status_name(err.status), err.message.c_str());
    return kRuntimeError;
  }

  std::printf("%zu class(es):\n", dex->classes().size());
  for (const aclcxx::dex::DexClass& c : dex->classes()) {
    std::printf("  class %s%s%s\n", c.java_name.c_str(),
                c.super_name ? " extends " : "",
                c.super_name ? c.super_name->c_str() : "");
    dump_methods("direct", c.direct_methods);
    dump_methods("virtual", c.virtual_methods);
  }
  return kOk;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);

  if (args.empty()) {
    print_version();
    print_roster();
    return kOk;
  }

  const std::string& cmd = args.front();
  if (cmd == "-h" || cmd == "--help") {
    print_usage();
    return kOk;
  }
  if (cmd == "-v" || cmd == "--version") {
    print_version();
    return kOk;
  }
  if (cmd == "run") {
    return cmd_run({args.begin() + 1, args.end()});
  }
  if (cmd == "dex") {
    return cmd_dex({args.begin() + 1, args.end()});
  }

  std::fprintf(stderr, "aclcxx: unknown command '%s'\n\n", cmd.c_str());
  print_usage();
  return kUsage;
}

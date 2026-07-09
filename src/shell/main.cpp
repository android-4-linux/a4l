// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/shell.md (pending), Android SDK launch semantics, xdg-desktop specs. No GPL-3.0 sources.
//
// aclcxx CLI. `aclcxx run <app.apk>` drives the Phase 1 runtime-spine vertical:
// open the APK (pkg), inspect its DEX/ABIs, configure a VM, and invoke a static
// main() through the runtime seam. The ART/JVM backends report Unavailable until
// they land (R-1 / ADR-0001); `--vm=null` exercises the full plumbing today.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "aclcxx_config.h"
#include "log/log.h"
#include "pkg/apk.h"
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
      "  (no command)         print version and build profile\n"
      "\n"
      "run options:\n"
      "  --vm=<art|jvm|null>  runtime backend (default: $ACLCXX_VM or art)\n"
      "  --main=<class>       static main() class to invoke\n"
      "  --classpath=<paths>  extra classpath entries (colon-separated); the JVM\n"
      "                       backend runs .class/.jar from here (DEX is not loaded\n"
      "                       by a stock JVM)\n"
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

  for (const std::string& a : args) {
    std::string v;
    if (match_opt(a, "--vm", v)) {
      vm_name = v;
    } else if (match_opt(a, "--main", v)) {
      main_class = v;
    } else if (match_opt(a, "--classpath", v)) {
      // Colon-separated, appended in order.
      for (std::size_t start = 0; start <= v.size();) {
        const std::size_t colon = v.find(':', start);
        const std::size_t end = colon == std::string::npos ? v.size() : colon;
        if (end > start) extra_classpath.push_back(v.substr(start, end - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
      }
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

  // Configure the runtime. The APK is passed on the classpath; extraction to a
  // real DEX/OAT cache is a later step (needs the framework boot classpath).
  r::VmConfig cfg;
  cfg.classpath = {apk_path};
  cfg.classpath.insert(cfg.classpath.end(), extra_classpath.begin(),
                       extra_classpath.end());
  cfg.cache_dir = cache_dir;
  // boot_classpath stays empty until the vendored framework lands.

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
    // Real launch-class resolution needs the (binary-XML) manifest, which is the
    // next pkg/ step. For now require --main to actually invoke.
    aclcxx::log::w(kTag,
                   "no --main given; manifest launch-class resolution is not "
                   "implemented yet. VM started; nothing invoked.");
  } else if (r::Error e = vm->call_static_main(main_class, {})) {
    aclcxx::log::e(kTag, "invoke {}.main failed: [{}] {}", main_class,
                   r::status_name(e.status), e.message);
    vm->shutdown();
    return kRuntimeError;
  }

  vm->shutdown();
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

  std::fprintf(stderr, "aclcxx: unknown command '%s'\n\n", cmd.c_str());
  print_usage();
  return kUsage;
}

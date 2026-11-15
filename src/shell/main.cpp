// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/shell.md (pending), Android SDK launch semantics,
// xdg-desktop specs. No GPL-3.0 sources.
//
// acl CLI. `acl run <app.apk>` drives the Phase 1 runtime-spine vertical:
// open the APK (pkg), inspect its DEX/ABIs, configure a VM, and invoke a static
// main() through the runtime seam. The ART/JVM backends report Unavailable
// until they land (R-1 / ADR-0001); `--vm=null` exercises the full plumbing
// today.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>  // getpid, for a per-process DEX conversion work dir

#include "acl_config.h"
#include "audio/audio.h"
#include "confine/confine.h"
#include "confine/seccomp_filter.h"
#include "dex/dex_to_jvm.h"
#include "dex/dexfile.h"
#include "log/log.h"
#include "pkg/apk.h"
#include "pkg/apk_stage.h"
#include "pkg/apk_verify.h"
#include "pkg/manifest.h"
#include "runtime/vm.h"
#include "wm/present.h"
#include "wm/split_present.h"

extern char** environ;

namespace {

constexpr std::string_view kTag = "acl";

// Exit codes.
enum : int { kOk = 0, kUsage = 1, kBackendUnavailable = 2, kRuntimeError = 3 };

void print_version() {
  std::printf("a4l %s\n", ACL_VERSION);
}

void print_roster() {
  // Phase 0 smoke: prove the native-core module link graph resolves.
  namespace r = acl::runtime;
  std::printf("profile: %s\n", ACL_PROFILE_AUTOMOTIVE ? "automotive" : "phone");
  std::printf(
      "runtime backends: art (target), jvm (fallback), null (plumbing)\n");
  (void)r::backend_name(r::Backend::Art);
}

void print_usage() {
  std::printf(
      "usage: a4l <command> [options]\n"
      "\n"
      "commands:\n"
      "  run <app.apk>        inspect and launch an APK\n"
      "  dex <file>           dump classes/methods of a .dex or .apk\n"
      "  verify <app.apk>     check the APK's v2/v3 signature and print its "
      "signer\n"
      "                       certificate digest; exit 0 only if it verifies\n"
      "  tone [options]       play a test tone through the audio backend "
      "(manual\n"
      "                       audio check); streams to PipeWire, else silent "
      "null sink\n"
      "  (no command)         print version and build profile\n"
      "\n"
      "run options:\n"
      "  --vm=<art|jvm|null>  runtime backend (default: $ACL_VM or art)\n"
      "  --main=<class>       static main() class to invoke\n"
      "  --service=<class>    host a MediaBrowserService (automotive media "
      "path)\n"
      "  --cal=<class>        host a CarAppService (automotive Car App Library "
      "path)\n"
      "  --classpath=<paths>  extra classpath entries (colon-separated); the "
      "JVM\n"
      "                       backend runs .class/.jar from here (DEX is not "
      "loaded\n"
      "                       by a stock JVM)\n"
      "  --convert-dex        convert the APK's DEX to JVM classes and add "
      "them to\n"
      "                       the classpath (implied by --vm=jvm)\n"
      "  --framework=<paths>  vendored android.* framework boot classpath "
      "(colon-\n"
      "                       separated); also read from $ACL_FRAMEWORK\n"
      "  --cache=<dir>        DEX->OAT / dalvik-cache directory\n"
      "  --verify=<mode>      APK signature policy (default: $ACL_APK_VERIFY "
      "or warn)\n"
      "                       warn: refuse a signature that fails, run an "
      "unsigned APK\n"
      "                       require: run only an APK whose signature "
      "verifies\n"
      "                       off: check nothing\n"
      "  --signer=<sha256>    run only an APK signed by this certificate "
      "(comma-\n"
      "                       separated list; also $ACL_APK_SIGNER); implies "
      "require\n"
      "  --split              run the app in a confined child process; this "
      "one keeps\n"
      "                       the display, input and APK verification (GPU "
      "present\n"
      "                       only). The child is sandboxed with bubblewrap "
      "and a\n"
      "                       seccomp filter: no home directory, no session, "
      "no\n"
      "                       network. $ACL_SPLIT_RO adds read-only paths\n"
      "  --split=unconfined   the same split with nothing confining the "
      "child\n"
      "  --net=host           with --split: share the host's network (and "
      "with it\n"
      "                       every local service and abstract socket)\n"
      "  --present            show the rendered content view in a Wayland "
      "window\n"
      "                       (wcs); no-op if no compositor / built without "
      "wcs\n"
      "  --present-frames=<n> present continuously for n frames via a "
      "Choreographer\n"
      "                       loop (implies --present; 0 = until the window "
      "closes)\n"
      "\n"
      "tone options:\n"
      "  --freq=<hz>          tone frequency (default 440)\n"
      "  --seconds=<s>        duration (default 2)\n"
      "  --rate=<hz>          sample rate (default 48000)\n"
      "  --channels=<n>       channel count (default 2)\n"
      "  --amplitude=<0..1>   volume, fraction of full scale (default 0.6)\n"
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

// Android ends an app by killing its process (Runtime.halt / SIGKILL), never by
// tearing down the ART VM. A graceful DestroyJavaVM (vm->shutdown()) — or a
// normal exit() running atexit / static dtors — races the still-live in-process
// native threads (Chromium's ThreadPool): as the VM/process winds down they
// call AttachCurrentThread, which returns JNI_ERR, and Chromium's
// CHECK_EQ(JNI_OK) aborts (base/android/jni_android.cc) -> SIGTRAP, which ART's
// fault handler then reenters ("HandleUnexpected Signal reentered") and HANGS.
// So end a run the Android way: skip the VM teardown and hard-exit the process
// (the OS reclaims everything), both on normal completion (end_run) and on a
// termination signal (acl_on_kill_signal, installed once the app is running).
void acl_on_kill_signal(int /*sig*/) {
  std::_Exit(0);
}  // async-signal-safe: just the exit syscall
[[noreturn]] void end_run(int code) {
  std::fflush(
      nullptr);  // flush app stdout/stderr (safe here — not a signal context)
  std::_Exit(code);
}

// Split a comma-separated list of certificate digests into `out`. Returns
// false, naming the bad one, if any is not a SHA-256 digest: a pin that can
// never match must not look like a pin that did not match.
bool split_signers(std::string_view v,
                   std::vector<std::string>& out,
                   std::string& bad) {
  while (!v.empty()) {
    const std::size_t comma = v.find(',');
    const std::string_view one = v.substr(0, comma);
    if (!one.empty()) {
      if (!acl::pkg::valid_cert_digest(one)) {
        bad = std::string(one);
        return false;
      }
      out.emplace_back(one);
    }
    if (comma == std::string_view::npos)
      break;
    v.remove_prefix(comma + 1);
  }
  return true;
}

// Log what a signature check found and decide whether the APK may run.
bool report_and_admit(const acl::pkg::SigResult& sig,
                      const acl::pkg::VerifyPolicy& policy,
                      long long ms) {
  namespace pkg = acl::pkg;
  const bool admit = pkg::admits(policy, sig);
  if (sig.verified()) {
    for (const pkg::ApkSigner& s : sig.signers) {
      acl::log::i(kTag, "signature: verified {} in {} ms, signer sha256={}",
                  pkg::sig_scheme_name(sig.scheme), ms, s.cert_sha256);
    }
    if (!admit) {
      acl::log::e(kTag,
                  "signature: signer is not one of the {} pinned "
                  "certificate(s); refusing to run",
                  policy.signers.size());
    }
    return admit;
  }
  if (!admit) {
    acl::log::e(kTag, "signature: {} ({}); refusing to run",
                pkg::sig_status_name(sig.status), sig.message);
    return false;
  }
  acl::log::w(kTag, "signature: {} ({}); running it anyway (--verify=warn)",
              pkg::sig_status_name(sig.status), sig.message);
  return true;
}

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// True if the policy needs a signature to hold, as opposed to reporting on it.
bool enforces(const acl::pkg::VerifyPolicy& policy) {
  return policy.mode == acl::pkg::VerifyMode::Require ||
         !policy.signers.empty();
}

// Check `apk` in place and decide whether it may run. This is the reporting
// path (--verify=warn or off): the verdict describes the file as it was read
// here, and the runtime reads the same path again later.
bool admit_in_place(const acl::pkg::Apk& apk,
                    const acl::pkg::VerifyPolicy& policy) {
  if (policy.mode == acl::pkg::VerifyMode::Off) {
    acl::log::w(kTag, "signature: not checked (--verify=off)");
    return true;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const acl::pkg::SigResult sig = acl::pkg::verify_signature(apk);
  return report_and_admit(sig, policy, ms_since(t0));
}

// The enforcing path (--verify=require or a pinned --signer): copy the APK
// into a4l's private cache, verify the copy, and replace `apk` and `apk_path`
// with the copy, so that everything the runtime reads afterwards is what was
// verified (ADR-0009 C1). Fills `lib_dir` with where its native libraries go.
bool admit_verified_copy(std::optional<acl::pkg::Apk>& apk,
                         std::string& apk_path,
                         const acl::pkg::VerifyPolicy& policy,
                         std::string& lib_dir) {
  namespace pkg = acl::pkg;
  std::string error;
  const std::string root = pkg::private_cache_root(error);
  if (root.empty()) {
    acl::log::e(kTag,
                "signature: no private cache to verify into ({}); "
                "refusing to run",
                error);
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  pkg::StageResult res = pkg::stage_verified(*apk, root);
  if (!res.error.empty()) {
    acl::log::e(kTag, "signature: {}; refusing to run", res.error);
    return false;
  }
  if (!report_and_admit(res.sig, policy, ms_since(t0)))
    return false;
  if (!res.staged) {  // admitted but not staged: the policy would be hollow
    acl::log::e(kTag, "signature: no verified copy was made; refusing to run");
    return false;
  }
  apk_path = res.staged->dir + "/base.apk";
  lib_dir = res.staged->lib_dir;
  acl::log::i(kTag, "signature: running the verified copy {} ({})", apk_path,
              res.staged->reused ? "reused" : "copied");
  // res.staged->lock_fd stays open on purpose: it marks the copy as in use
  // until this process exits.
  apk = std::move(res.staged->apk);
  return true;
}

int cmd_verify(const std::vector<std::string>& args) {
  namespace pkg = acl::pkg;
  std::string path;
  std::vector<std::string> pins;
  for (const std::string& a : args) {
    std::string v;
    if (match_opt(a, "--signer", v)) {
      std::string bad;
      if (!split_signers(v, pins, bad)) {
        std::fprintf(stderr, "verify: '%s' is not a SHA-256 digest\n",
                     bad.c_str());
        return kUsage;
      }
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "verify: unknown option '%s'\n", a.c_str());
      return kUsage;
    } else if (path.empty()) {
      path = a;
    } else {
      std::fprintf(stderr, "verify: unexpected argument '%s'\n", a.c_str());
      return kUsage;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "verify: missing <app.apk>\n");
    return kUsage;
  }
  pkg::ZipError zerr;
  auto apk = pkg::Apk::open(path, zerr);
  if (!apk.has_value()) {
    std::fprintf(stderr, "verify: cannot open %s: [%s] %s\n", path.c_str(),
                 pkg::zip_status_name(zerr.status), zerr.message.c_str());
    return kUsage;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const pkg::SigResult sig = pkg::verify_signature(*apk);
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  std::printf("status: %s\n", pkg::sig_status_name(sig.status));
  if (!sig.verified()) {
    std::printf("reason: %s\n", sig.message.c_str());
    return kRuntimeError;
  }
  std::printf("scheme: %s\n", pkg::sig_scheme_name(sig.scheme));
  for (const pkg::ApkSigner& s : sig.signers)
    std::printf("signer: %s\n", s.cert_sha256.c_str());
  if (sig.has_rotation_lineage)
    std::printf("lineage: present, not validated\n");
  std::printf("bytes: %zu\ntime_ms: %.1f\n", apk->bytes().size(), ms);
  for (const std::string& pin : pins) {
    if (pkg::signer_matches(sig, pin))
      return kOk;
  }
  if (!pins.empty()) {
    std::printf("pinned: no match\n");
    return kRuntimeError;
  }
  return kOk;
}

// --split, the broker side (ADR-0009): run the app in a child a4l process and
// keep the display, the input devices and the APK verification here. The
// child is this binary with ACL_SPLIT_FD set; its presenter is a proxy over
// that socket (wm/split_present.h). `apk_path` is the file already admitted,
// the verified copy when the policy enforces; the child is told not to verify
// again.
// A colon-separated list of paths with every relative one made absolute. A
// confined child starts in "/", so a path relative to this process's
// directory would not name the same file there.
std::string absolute_list(std::string_view list) {
  std::string out;
  while (!list.empty()) {
    const std::size_t c = list.find(':');
    const std::string_view part = list.substr(0, c);
    if (!part.empty()) {
      if (!out.empty())
        out += ':';
      out += std::filesystem::absolute(part).lexically_normal().string();
    }
    if (c == std::string_view::npos)
      break;
    list.remove_prefix(c + 1);
  }
  return out;
}

struct SplitOptions {
  bool confine = true;     // --split, as opposed to --split=unconfined
  bool share_net = false;  // --net=host
  std::vector<std::string> ro_paths;  // --framework, --classpath
  std::vector<std::string> rw_paths;  // --cache
};

int run_split(const acl::pkg::Apk& apk,
              const std::string& apk_path,
              const std::vector<std::string>& args,
              const SplitOptions& opt) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
  char self[4096];
  const ssize_t self_len = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (self_len <= 0) {
    acl::log::e(kTag, "split: cannot resolve /proc/self/exe");
    return kRuntimeError;
  }
  self[self_len] = '\0';

  int dw = 0, dh = 0, dscale = 1;
  if (!acl::wm::query_display(dw, dh, dscale))
    dw = dh = 0;  // no compositor: the child runs headless

  auto [broker_end, child_end] = acl::ipc::Channel::make_pair();
  if (!broker_end.valid()) {
    acl::log::e(kTag, "split: socketpair failed: {}", std::strerror(errno));
    return kRuntimeError;
  }

  // The child's command line: this one, minus what the broker already did.
  std::vector<std::string> argv_s = {
      self, "run",
      std::filesystem::absolute(apk_path).lexically_normal().string()};
  bool positional_seen = false;
  for (const std::string& a : args) {
    if (a.rfind("--split", 0) == 0 || a.rfind("--verify", 0) == 0 ||
        a.rfind("--signer", 0) == 0 || a.rfind("--net", 0) == 0)
      continue;
    if (!a.empty() && a[0] != '-' && !positional_seen) {
      positional_seen = true;  // the APK path, replaced above
      continue;
    }
    bool rewritten = false;
    for (const std::string_view key :
         {"--framework=", "--classpath=", "--cache="}) {
      if (a.rfind(key, 0) == 0) {
        argv_s.push_back(std::string(key) +
                         absolute_list(std::string_view(a).substr(key.size())));
        rewritten = true;
      }
    }
    if (!rewritten)
      argv_s.push_back(a);
  }
  argv_s.emplace_back("--verify=off");

  // The package name, from the APK this process admitted. It names the app's
  // directories and its window; neither is the child's to choose.
  acl::pkg::Manifest manifest;
  acl::pkg::ZipError merr;
  if (auto mbytes = apk.read("AndroidManifest.xml", merr))
    (void)acl::pkg::parse_manifest(*mbytes, manifest);
  const std::string package_dir = acl::pkg::safe_package_dir(manifest.package);

  // Confinement (ADR-0009 C4): the plan for bubblewrap, which becomes the
  // program this process starts, and the child's environment as an allowlist.
  static constexpr int kChildFd = 3;
  std::vector<std::string> env_s;
  if (opt.confine) {
    acl::confine::SeccompMode mode{};
    const char* mode_text = std::getenv("ACL_SPLIT_SECCOMP");
    if (!acl::confine::parse_seccomp_mode(mode_text ? mode_text : "", mode)) {
      acl::log::e(kTag, "split: bad $ACL_SPLIT_SECCOMP '{}' (errno|kill|off)",
                  mode_text);
      return kUsage;
    }
    if (mode != acl::confine::SeccompMode::Off &&
        acl::confine::allowed_syscalls().empty()) {
      acl::log::e(kTag,
                  "split: a4l has no system call list for this architecture, "
                  "so it cannot confine the app process; --split=unconfined "
                  "runs it without confinement");
      return kBackendUnavailable;
    }
    acl::confine::Inputs in;
    in.apk = argv_s[2];
    in.package_dir = package_dir;
    in.ro_paths = opt.ro_paths;
    in.rw_paths = opt.rw_paths;
    in.share_net = opt.share_net;
    acl::confine::Launch launch;
    std::string why;
    if (!acl::confine::plan_launch(in, environ, launch, why)) {
      acl::log::e(kTag,
                  "split: cannot confine the app process: {}; "
                  "--split=unconfined runs it without confinement",
                  why);
      return kBackendUnavailable;
    }
    const std::vector<std::string> wrap = acl::confine::bwrap_argv(launch.plan);
    argv_s.insert(argv_s.begin(), wrap.begin(), wrap.end());
    env_s = std::move(launch.env);
    env_s.push_back("ACL_SPLIT_CONFINED=" +
                    acl::confine::describe_namespaces(!opt.share_net));
    acl::log::i(kTag,
                "split: confining the app process: {} read-only path(s), "
                "data {}, cache {}, {} GPU node(s), network {}, seccomp {}",
                launch.plan.ro.size(), launch.data_dir, launch.cache_dir,
                launch.plan.devices.size(),
                opt.share_net ? "shared with the host" : "none",
                mode == acl::confine::SeccompMode::Off    ? "OFF"
                : mode == acl::confine::SeccompMode::Kill ? "kill"
                                                          : "errno");
    if (mode == acl::confine::SeccompMode::Off)
      acl::log::w(kTag,
                  "split: $ACL_SPLIT_SECCOMP=off: the app process has "
                  "every system call");
    if (opt.share_net)
      acl::log::w(kTag,
                  "split: --net=host: the app process can reach every "
                  "abstract socket and local service of this host, "
                  "an X server among them");
  } else {
    // Unconfined: the caller's environment, minus the route to the
    // compositor and what the broker decides.
    static constexpr std::string_view kDropped[] = {
        "WAYLAND_DISPLAY=",  "WAYLAND_SOCKET=",    "ACL_APK_VERIFY=",
        "ACL_APK_SIGNER=",   "ACL_SPLIT_FD=",      "ACL_SPLIT_DISPLAY=",
        "ACL_SPLIT_BROKER=", "ACL_SPLIT_CONFINED="};
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
      const std::string_view v(*e);
      bool drop = false;
      for (const std::string_view d : kDropped)
        drop = drop || v.rfind(d, 0) == 0;
      if (!drop)
        env_s.emplace_back(v);
    }
    env_s.push_back("ACL_SPLIT_BROKER=" + std::to_string(::getpid()));
    acl::log::w(kTag,
                "split: --split=unconfined: nothing confines the app "
                "process");
  }
  bool gpu_set = false;
  for (const std::string& e : env_s)
    gpu_set = gpu_set || e.rfind("ACL_PRESENT_GPU=", 0) == 0;
  env_s.push_back("ACL_SPLIT_FD=" + std::to_string(kChildFd));
  env_s.push_back("ACL_SPLIT_DISPLAY=" + std::to_string(dw) + "x" +
                  std::to_string(dh) + "@" + std::to_string(dscale));
  if (!gpu_set)
    env_s.emplace_back("ACL_PRESENT_GPU=1");  // the path that crosses

  std::vector<char*> argv, envp;
  for (std::string& a : argv_s)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  for (std::string& e : env_s)
    envp.push_back(e.data());
  envp.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, child_end.native_handle(), kChildFd);
  // The child inherits the socket and the standard streams, nothing else
  // this process happens to hold open. A confined child gets no terminal
  // input either.
  posix_spawn_file_actions_addclosefrom_np(&fa, kChildFd + 1);
  if (opt.confine)
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  pid_t pid = 0;
  const int rc =
      ::posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  child_end.close();
  if (rc != 0) {
    acl::log::e(kTag, "split: posix_spawn failed: {}", std::strerror(rc));
    return kRuntimeError;
  }
  acl::log::i(kTag, "split: app process {} started; this process is the broker",
              pid);

  acl::wm::BrokerOptions broker_options;
  // "app" is safe_package_dir()'s answer for an APK with no usable package
  // name; such a window keeps the id the child gives, cleaned.
  if (package_dir != "app")
    broker_options.app_id = package_dir;
  const acl::wm::BrokerStats stats = acl::wm::serve_split_child(
      broker_end, acl::wm::presenter_open, broker_options);
  broker_end.close();

  // The child leaves on its own when the socket closes. Give it 3 s.
  int wstatus = 0;
  bool reaped = false;
  for (int i = 0; i < 150 && !reaped; ++i) {
    reaped = ::waitpid(pid, &wstatus, WNOHANG) == pid;
    if (!reaped) {
      const timespec ts{0, 20'000'000};
      ::nanosleep(&ts, nullptr);
    }
  }
  if (!reaped) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &wstatus, 0);
  }
  // Bubblewrap reports a child killed by signal N as exit status 128 + N.
  const bool signaled =
      reaped && (WIFSIGNALED(wstatus) || (opt.confine && WIFEXITED(wstatus) &&
                                          WEXITSTATUS(wstatus) > 128));
  acl::log::i(kTag,
              "split: app process ended ({}); {} frame(s) presented, {} input "
              "event(s) forwarded, {} message(s) rejected, {} datagram(s) from "
              "the app and {} to it, {} video frame(s) shown",
              !reaped              ? "killed after the window closed"
              : signaled           ? "signaled"
              : WIFEXITED(wstatus) ? "exited"
                                   : "stopped",
              stats.frames, stats.inputs, stats.rejected, stats.from_child,
              stats.to_child, stats.video_frames);
  if (!reaped || stats.window_closed)
    return kOk;
  if (WIFEXITED(wstatus) && !signaled)
    return WEXITSTATUS(wstatus);
  return kRuntimeError;
#else
  (void)apk;
  (void)apk_path;
  (void)args;
  (void)opt;
  acl::log::e(kTag,
              "split: this build has no GPU present path (needs Vulkan and "
              "wcs); --split is unavailable");
  return kBackendUnavailable;
#endif
}

// The child side of --split: adopt the broker's socket before anything opens
// a display. Returns false if ACL_SPLIT_FD is set but unusable.
bool adopt_split_child() {
  const char* e = std::getenv("ACL_SPLIT_FD");
  if (e == nullptr || *e == '\0')
    return true;
  const int fd = std::atoi(e);
  struct stat st{};
  if (fd < 3 || ::fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
    std::fprintf(stderr, "a4l: $ACL_SPLIT_FD '%s' is not a socket\n", e);
    return false;
  }
  int w = 0, h = 0, scale = 1;
  if (const char* d = std::getenv("ACL_SPLIT_DISPLAY"))
    std::sscanf(d, "%dx%d@%d", &w, &h, &scale);
  // Die with the broker. If it went away before the prctl took effect, the
  // parent is no longer the pid it gave. A confined child's parent is
  // bubblewrap, which dies with the broker itself.
  ::prctl(PR_SET_PDEATHSIG, SIGKILL);
  const char* confined = std::getenv("ACL_SPLIT_CONFINED");
  const char* broker = std::getenv("ACL_SPLIT_BROKER");
  if (confined == nullptr && broker != nullptr && *broker != '\0' &&
      ::getppid() != static_cast<pid_t>(std::atol(broker))) {
    std::fprintf(stderr, "a4l: the split broker is gone\n");
    return false;
  }
  // Confined (ADR-0009 C4): install the system call filter now, before the
  // APK is opened, and refuse to go on unless this process is where the
  // broker meant it to be.
  if (confined != nullptr) {
    acl::confine::SeccompMode mode{};
    const char* mode_text = std::getenv("ACL_SPLIT_SECCOMP");
    std::string why;
    if (!acl::confine::parse_seccomp_mode(mode_text ? mode_text : "", mode)) {
      why = "bad $ACL_SPLIT_SECCOMP";
    } else if (mode != acl::confine::SeccompMode::Off) {
      acl::confine::install_filter(mode == acl::confine::SeccompMode::Kill
                                       ? acl::confine::Deny::Kill
                                       : acl::confine::Deny::Errno,
                                   why);
    }
    if (why.empty()) {
      acl::confine::SelfCheck check;
      check.host_namespaces = confined;
      check.ipc_fd = fd;
      check.expect_filter = mode != acl::confine::SeccompMode::Off;
      why = acl::confine::verify_confined(check);
    }
    if (!why.empty()) {
      std::fprintf(stderr, "a4l: the app process is not confined: %s\n",
                   why.c_str());
      return false;
    }
    acl::log::i(kTag,
                "split: app process confined ({}system call filter, "
                "own user, mount, pid and ipc namespaces)",
                mode == acl::confine::SeccompMode::Off ? "NO " : "");
    ::unsetenv("ACL_SPLIT_CONFINED");
  }
  ::unsetenv("ACL_SPLIT_FD");
  ::unsetenv("WAYLAND_DISPLAY");
  acl::wm::split_child_install(fd, w, h, scale);
  return true;
}

int cmd_run(const std::vector<std::string>& args) {
  namespace r = acl::runtime;

  std::string apk_path;
  std::string vm_name;
  std::string main_class;
  std::string service_class;
  std::string cal_class;
  std::string cache_dir;
  std::vector<std::string> extra_classpath;
  std::vector<std::string> framework_dirs;
  bool convert_dex = false;
  bool present = false;
  bool split = false;
  SplitOptions split_opt;
  bool net_flag_seen = false;
  std::string present_frames;
  acl::pkg::VerifyPolicy verify_policy;
  if (const char* e = std::getenv("ACL_APK_VERIFY"); e && *e) {
    if (!acl::pkg::parse_verify_mode(e, verify_policy.mode)) {
      std::fprintf(stderr, "run: bad $ACL_APK_VERIFY '%s' (off|warn|require)\n",
                   e);
      return kUsage;
    }
  }
  if (const char* e = std::getenv("ACL_APK_SIGNER"); e && *e) {
    std::string bad;
    if (!split_signers(e, verify_policy.signers, bad)) {
      std::fprintf(stderr,
                   "run: $ACL_APK_SIGNER '%s' is not a SHA-256 digest\n",
                   bad.c_str());
      return kUsage;
    }
  }
  bool signer_flag_seen = false;

  // Split a colon-separated path list into `out`, skipping empties.
  const auto split_paths = [](const std::string& v,
                              std::vector<std::string>& out) {
    for (std::size_t start = 0; start <= v.size();) {
      const std::size_t colon = v.find(':', start);
      const std::size_t end = colon == std::string::npos ? v.size() : colon;
      if (end > start)
        out.push_back(v.substr(start, end - start));
      if (colon == std::string::npos)
        break;
      start = colon + 1;
    }
  };

  for (const std::string& a : args) {
    std::string v;
    if (a == "--convert-dex") {
      convert_dex = true;
    } else if (a == "--present") {
      present = true;
    } else if (a == "--split") {
      split = true;
    } else if (match_opt(a, "--split", v)) {
      if (v != "confined" && v != "unconfined") {
        std::fprintf(stderr, "run: bad --split '%s' (confined|unconfined)\n",
                     v.c_str());
        return kUsage;
      }
      split = true;
      split_opt.confine = v == "confined";
    } else if (match_opt(a, "--net", v)) {
      if (v != "none" && v != "host") {
        std::fprintf(stderr, "run: bad --net '%s' (none|host)\n", v.c_str());
        return kUsage;
      }
      net_flag_seen = true;
      split_opt.share_net = v == "host";
    } else if (match_opt(a, "--present-frames", v)) {
      present = true;
      present_frames = v;
    } else if (match_opt(a, "--vm", v)) {
      vm_name = v;
    } else if (match_opt(a, "--framework", v)) {
      split_paths(v, framework_dirs);  // vendored android.* boot classpath
    } else if (match_opt(a, "--service", v)) {
      service_class = v;
    } else if (match_opt(a, "--cal", v)) {
      cal_class = v;
    } else if (match_opt(a, "--main", v)) {
      main_class = v;
    } else if (match_opt(a, "--classpath", v)) {
      split_paths(v, extra_classpath);  // colon-separated, appended in order
    } else if (match_opt(a, "--cache", v)) {
      cache_dir = v;
    } else if (match_opt(a, "--verify", v)) {
      if (!acl::pkg::parse_verify_mode(v, verify_policy.mode)) {
        std::fprintf(stderr, "run: bad --verify '%s' (off|warn|require)\n",
                     v.c_str());
        return kUsage;
      }
    } else if (match_opt(a, "--signer", v)) {
      if (!signer_flag_seen)
        verify_policy.signers.clear();  // the flag replaces $ACL_APK_SIGNER
      signer_flag_seen = true;
      std::string bad;
      if (!split_signers(v, verify_policy.signers, bad)) {
        std::fprintf(stderr, "run: --signer '%s' is not a SHA-256 digest\n",
                     bad.c_str());
        return kUsage;
      }
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
    std::fprintf(stderr, "run: unknown --vm '%s' (art|jvm|null)\n",
                 vm_name.c_str());
    return kUsage;
  }

  // Open and inspect the APK.
  acl::pkg::ZipError zerr;
  auto apk = acl::pkg::Apk::open(apk_path, zerr);
  if (!apk.has_value()) {
    acl::log::e(kTag, "cannot open {}: [{}] {}", apk_path,
                acl::pkg::zip_status_name(zerr.status), zerr.message);
    return kUsage;
  }
  // With an enforcing policy the APK is replaced by a private, verified copy
  // here, before the manifest is parsed or anything is extracted.
  std::string verified_lib_dir;
  if (enforces(verify_policy)
          ? !admit_verified_copy(apk, apk_path, verify_policy, verified_lib_dir)
          : !admit_in_place(*apk, verify_policy)) {
    return kRuntimeError;
  }
  // The broker stops here: the APK is admitted, and everything it supplies
  // runs in the child.
  if (net_flag_seen && !(split && split_opt.confine)) {
    std::fprintf(stderr, "run: --net applies to a confined --split only\n");
    return kUsage;
  }
  if (split) {
    for (const auto* list : {&framework_dirs, &extra_classpath}) {
      for (const std::string& p : *list)
        split_opt.ro_paths.push_back(absolute_list(p));
    }
#ifdef ACL_DEFAULT_FRAMEWORK
    split_opt.ro_paths.emplace_back(ACL_DEFAULT_FRAMEWORK);
#endif
#ifdef ACL_DEFAULT_ANDROID_JAR
    split_opt.ro_paths.emplace_back(ACL_DEFAULT_ANDROID_JAR);
#endif
    if (!cache_dir.empty())
      split_opt.rw_paths.push_back(absolute_list(cache_dir));
    return run_split(*apk, apk_path, args, split_opt);
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

  acl::log::i(kTag, "opened {} ({} entries, {} dex, abis=[{}])", apk_path,
              apk->entries().size(), dex_files.size(), [&] {
                std::string s;
                for (std::size_t i = 0; i < abis.size(); ++i) {
                  if (i)
                    s += ',';
                  s += abis[i];
                }
                return s;
              }());
  if (dex_files.empty()) {
    acl::log::w(kTag, "no classes*.dex found — nothing to execute");
  }

  // Parse the manifest for package identity and the launch class.
  acl::pkg::Manifest manifest;
  acl::pkg::ZipError merr;
  if (auto mbytes = apk->read("AndroidManifest.xml", merr)) {
    if (acl::pkg::AxmlError ae = acl::pkg::parse_manifest(*mbytes, manifest)) {
      acl::log::w(kTag, "manifest parse failed: [{}] {}",
                  acl::pkg::axml_status_name(ae.status), ae.message);
    } else {
      acl::log::i(kTag, "package={} versionCode={} minSdk={} targetSdk={}",
                  manifest.package.empty() ? "?" : manifest.package,
                  manifest.version_code ? *manifest.version_code : 0,
                  manifest.min_sdk ? *manifest.min_sdk : -1,
                  manifest.target_sdk ? *manifest.target_sdk : -1);
    }
  } else {
    acl::log::w(kTag, "no AndroidManifest.xml: [{}] {}",
                acl::pkg::zip_status_name(merr.status), merr.message);
  }

  // Resolve the launch class: explicit --main wins, else the manifest launcher.
  if (main_class.empty()) {
    if (const acl::pkg::Activity* launch = manifest.launcher()) {
      // launch_class() follows an <activity-alias> to its real targetActivity
      // (the alias name is not an instantiable class — e.g. Spotify's
      // com.spotify.music.MainActivity alias).
      main_class = launch->launch_class();
      acl::log::i(kTag, "launch activity: {}", main_class);
    }
  }

  // Configure the runtime. The APK is passed on the classpath; extraction to a
  // real DEX/OAT cache is a later step (needs the framework boot classpath).
  r::VmConfig cfg;
  cfg.classpath = {apk_path};
  cfg.classpath.insert(cfg.classpath.end(), extra_classpath.begin(),
                       extra_classpath.end());
  cfg.cache_dir = cache_dir;
  cfg.package_name =
      manifest.package;  // injected into the entry point's Context
  cfg.application_name =
      manifest
          .application_name;  // app Application, created before the Activity
  cfg.application_theme =
      manifest.application_theme;  // applied to the app Context's theme
  // A per-activity android:theme overrides the application theme for the
  // launched activity — many apps declare a NoActionBar activity theme so they
  // can setSupportActionBar their own Toolbar (the application theme keeps the
  // DarkActionBar). Prefer the launcher activity's theme when set.
  if (const acl::pkg::Activity* launch = manifest.launcher();
      launch != nullptr && launch->theme != 0) {
    cfg.application_theme = launch->theme;
    acl::log::i(kTag, "using launcher activity theme resId=0x{:x}",
                launch->theme);
  }
  cfg.present = present;  // show the rendered content view via wm/ (wcs)
  if (!present_frames.empty())
    cfg.present_frames = std::strtoull(present_frames.c_str(), nullptr, 10);

  // Native libraries (NDK): extract the app's lib/<abi>/*.so for this host's
  // ABI to a per-app dir and point java.library.path at it, so
  // System.loadLibrary(name) resolves lib<name>.so. An ABI mismatch (e.g. an
  // arm-only app on an x86_64 host) is reported, not extracted. Real apps ship
  // bionic-linked .so, which load only when host-arch-compatible AND through
  // the bionic bridge (Phase 4 / ADR-5, R-6); this wires the front of that
  // path. See docs/ndk-scope.md.
  if (!abis.empty()) {
    const std::string abi = acl::pkg::Apk::select_abi(abis);
    if (abi.empty()) {
      acl::log::w(
          kTag,
          "native libs present but no host-compatible ABI (see abis above); "
          "System.loadLibrary will fail (needs arch match + bionic bridge)");
    } else {
      // A verified copy keeps its libraries beside it. Otherwise they go
      // under a4l's cache by package name, which comes from the manifest and
      // is reduced to a safe directory name first.
      std::string libdir = verified_lib_dir;
      std::string lib_error;
      if (libdir.empty()) {
        const std::string root = acl::pkg::private_cache_root(lib_error);
        if (!root.empty()) {
          libdir = root + "/" + acl::pkg::safe_package_dir(manifest.package) +
                   "/lib";
        }
      }
      int nlibs = 0;
      if (libdir.empty() ||
          !acl::pkg::extract_native_libs(*apk, abi, libdir, nlibs, lib_error)) {
        if (enforces(verify_policy)) {
          acl::log::e(kTag, "native libs: {}; refusing to run", lib_error);
          return kRuntimeError;
        }
        acl::log::w(kTag, "native libs: {}; System.loadLibrary may fail",
                    lib_error);
      }
      cfg.options.push_back("-Djava.library.path=" + libdir);
      acl::log::i(kTag, "native libs: extracted {} lib(s) for abi '{}' -> {}",
                  nlibs, abi, libdir);
      // Hand the extracted dir to the bionic loader as an extra search path, so
      // an app lib's NEEDED on a *sibling* app lib resolves (the staged sysroot
      // only carries bionic itself). The loader is gated on ACL_BIONIC_LINKER;
      // setting this when it is unset is inert. An explicit ACL_BIONIC_APP_LIBS
      // from the environment wins (overwrite=0) so a caller can point the
      // search elsewhere. See jvm_vm register_ndk_loadlibrary_override +
      // docs/ndk-scope.md.
      ::setenv("ACL_BIONIC_APP_LIBS", libdir.c_str(), 0);
    }
  }

  // Boot classpath = the android.* framework (ADR-0002). Precedence:
  // --framework, then $ACL_FRAMEWORK, then the staged default (framework/ ->
  // build/framework/classes, baked in at build time) so `acl run <apk>`
  // resolves android.* out of the box.
  cfg.boot_classpath = framework_dirs;
  if (const char* env = std::getenv("ACL_FRAMEWORK")) {
    split_paths(env, cfg.boot_classpath);
  }
#ifdef ACL_DEFAULT_FRAMEWORK
  if (cfg.boot_classpath.empty() &&
      std::filesystem::exists(ACL_DEFAULT_FRAMEWORK)) {
    cfg.boot_classpath.push_back(
        ACL_DEFAULT_FRAMEWORK);  // staged framework/ classes
    // The api-33 android.jar stub layer resolves AFTER our shims (so our
    // classes with real behavior win) and fills the rest of android.*
    // (ADR-0002). $ACL_ANDROID_JAR overrides the baked-in default.
    std::string android_jar;
    if (const char* j = std::getenv("ACL_ANDROID_JAR"))
      android_jar = j;
#ifdef ACL_DEFAULT_ANDROID_JAR
    if (android_jar.empty())
      android_jar = ACL_DEFAULT_ANDROID_JAR;
#endif
    if (!android_jar.empty() && std::filesystem::exists(android_jar)) {
      cfg.boot_classpath.push_back(android_jar);
    }
  }
#endif
  if (!cfg.boot_classpath.empty()) {
    acl::log::i(kTag, "framework boot classpath: {} entr{}",
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
        acl::log::w(kTag, "convert-dex: cannot read {}", dex_name);
        continue;
      }
      acl::dex::DexError de;
      auto dx = acl::dex::DexFile::parse(*db, de);
      if (!dx) {
        acl::log::w(kTag, "convert-dex: parse {} failed: {}", dex_name,
                    de.message);
        continue;
      }
      // A work dir unique per process AND per DEX, so concurrent `acl run`
      // invocations (and multi-dex within one) never clobber each other's
      // classes.
      acl::dex::ConvertOptions opts;
      opts.work_dir = (std::filesystem::temp_directory_path() /
                       ("acl-" + std::to_string(::getpid()) + "-dex-" +
                        std::to_string(idx)))
                          .string();
      acl::dex::ConvertResult cr;
      if (acl::dex::DexError ce = acl::dex::convert(*dx, opts, cr)) {
        acl::log::w(kTag, "convert-dex: {} failed: {}", dex_name, ce.message);
        continue;
      }
      acl::log::i(kTag, "convert-dex: {} -> {} ({} classes)", dex_name,
                  cr.classpath_dir, cr.classes.size());
      cfg.classpath.push_back(cr.classpath_dir);
    }
  }

  r::Error err;
  auto vm = r::create_vm(backend, cfg, err);
  if (vm == nullptr) {
    acl::log::e(kTag, "runtime backend '{}' unavailable: {}",
                r::backend_name(backend), err.message);
    return kBackendUnavailable;
  }

  if (r::Error e = vm->start()) {
    acl::log::e(kTag, "vm start failed: [{}] {}", r::status_name(e.status),
                e.message);
    return kRuntimeError;
  }

  // The VM (and possibly in-process native runtimes like Chromium's WebView) is
  // now live. Terminate the process directly on a kill signal rather than
  // letting the default action / any handler race those native threads into a
  // JNI-on-a-dying-VM crash (see end_run). ART leaves SIGTERM/SIGINT at their
  // defaults, so this only overrides "terminate" with "terminate now, cleanly".
  std::signal(SIGTERM, acl_on_kill_signal);
  std::signal(SIGINT, acl_on_kill_signal);

  if (!service_class.empty()) {
    // Automotive media path: host a MediaBrowserService (no Views, no
    // launcher).
    if (r::Error e = vm->call_media_browser(service_class)) {
      acl::log::e(kTag, "host service {} failed: [{}] {}", service_class,
                  r::status_name(e.status), e.message);
      vm->shutdown();
      return kRuntimeError;
    }
  } else if (!cal_class.empty()) {
    // Automotive Car App Library path: host a CarAppService (template tree, no
    // Views).
    if (r::Error e = vm->call_car_app(cal_class)) {
      acl::log::e(kTag, "host CarAppService {} failed: [{}] {}", cal_class,
                  r::status_name(e.status), e.message);
      vm->shutdown();
      return kRuntimeError;
    }
  } else if (main_class.empty()) {
    acl::log::w(kTag,
                "no launcher activity in the manifest and no --main given; "
                "VM started, nothing invoked");
  } else {
    // Prefer the Android Activity lifecycle; fall back to a static main() for
    // self-contained apps (or when no framework Activity is on the boot
    // classpath).
    r::Error e = vm->call_activity_lifecycle(main_class);
    if (e.status == r::Status::NotApplicable) {
      acl::log::i(kTag, "{} is not an Activity; invoking static main()",
                  main_class);
      e = vm->call_static_main(main_class, {});
    }
    if (e) {
      acl::log::e(kTag, "launch {} failed: [{}] {}", main_class,
                  r::status_name(e.status), e.message);
      end_run(kRuntimeError);  // app may have spun up WebView/native threads —
                               // no DestroyJavaVM
    }
  }

  end_run(kOk);  // Android-style process exit (no DestroyJavaVM race with
                 // native threads)
}

void dump_methods(const char* kind,
                  const std::vector<acl::dex::DexMethod>& ms) {
  for (const acl::dex::DexMethod& m : ms) {
    std::string params;
    for (std::size_t i = 0; i < m.param_types.size(); ++i) {
      if (i)
        params += ", ";
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
  bytes.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
  const bool is_dex =
      bytes.size() >= 4 && std::memcmp(bytes.data(), "dex\n", 4) == 0;
  if (!is_dex) {
    acl::pkg::ZipError zerr;
    auto apk = acl::pkg::Apk::open(path, zerr);
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

  acl::dex::DexError err;
  auto dex = acl::dex::DexFile::parse(bytes, err);
  if (!dex.has_value()) {
    std::fprintf(stderr, "dex: parse failed: [%s] %s\n",
                 acl::dex::dex_status_name(err.status), err.message.c_str());
    return kRuntimeError;
  }

  std::printf("%zu class(es):\n", dex->classes().size());
  for (const acl::dex::DexClass& c : dex->classes()) {
    std::printf("  class %s%s%s\n", c.java_name.c_str(),
                c.super_name ? " extends " : "",
                c.super_name ? c.super_name->c_str() : "");
    dump_methods("direct", c.direct_methods);
    dump_methods("virtual", c.virtual_methods);
  }
  return kOk;
}

}  // namespace

int cmd_tone(const std::vector<std::string>& args) {
  acl::audio::ToneSpec spec;
  std::string v;
  for (const std::string& a : args) {
    if (match_opt(a, "--freq", v))
      spec.freq_hz = std::atoi(v.c_str());
    else if (match_opt(a, "--seconds", v))
      spec.seconds = std::atof(v.c_str());
    else if (match_opt(a, "--rate", v))
      spec.sample_rate = std::atoi(v.c_str());
    else if (match_opt(a, "--channels", v))
      spec.channels = std::atoi(v.c_str());
    else if (match_opt(a, "--amplitude", v))
      spec.amplitude = std::atof(v.c_str());
    else {
      std::fprintf(stderr, "a4l tone: unknown option '%s'\n", a.c_str());
      return kUsage;
    }
  }
  const std::uint64_t frames = acl::audio::play_tone(spec);
  acl::log::i(kTag, "tone: done, {} frames written", frames);
  return kOk;
}

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
    if (!adopt_split_child())
      return kRuntimeError;
    return cmd_run({args.begin() + 1, args.end()});
  }
  if (cmd == "dex") {
    return cmd_dex({args.begin() + 1, args.end()});
  }
  if (cmd == "verify") {
    return cmd_verify({args.begin() + 1, args.end()});
  }
  if (cmd == "tone") {
    return cmd_tone({args.begin() + 1, args.end()});
  }

  std::fprintf(stderr, "a4l: unknown command '%s'\n\n", cmd.c_str());
  print_usage();
  return kUsage;
}

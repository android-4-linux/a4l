// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md, S1). No GPL-3.0
// sources.
//
// The acl (glibc) side of the split: launch acl-art-host, feed it the
// classpath/main, and stream its output back over acl_ipc. S1 runs a static
// main() one-shot (art-host boots the VM, runs main, and exits); S2+ keeps
// art-host long-lived to serve frames/input.

#include "runtime/split_vm.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ipc/ipc.h"
#include "log/log.h"
#include "wm/present_sink.h"

extern char** environ;

namespace acl::runtime {
namespace {

constexpr std::string_view kTag = "acl.runtime.split";

std::string join(const std::vector<std::string>& parts, char sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i)
      out.push_back(sep);
    out += parts[i];
  }
  return out;
}

// Locate the acl-art-host executable: ACL_ART_HOST, else alongside this binary,
// else on PATH.
std::string art_host_path() {
  if (const char* p = std::getenv("ACL_ART_HOST"); p && *p)
    return p;
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    std::string s(buf);
    auto slash = s.find_last_of('/');
    if (slash != std::string::npos)
      return s.substr(0, slash + 1) + "acl-art-host";
  }
  return "acl-art-host";
}

// The runtime library art-host dlopens. Default to the host ART under
// ACL_ART_HOME; overridable (x86 dev/CI points this at an OpenJDK libjvm.so).
std::string vm_lib_path() {
  if (const char* p = std::getenv("ACL_ART_SPLIT_VMLIB"); p && *p)
    return p;
  if (const char* home = std::getenv("ACL_ART_HOME"); home && *home)
    return std::string(home) + "/lib64/libart.so";
  return "";
}

class SplitVm final : public Vm {
 public:
  explicit SplitVm(VmConfig config)
      : Vm(Backend::ArtSplit), config_(std::move(config)) {}

  Error start() override {
    started_ = true;  // art-host is launched per-call (one-shot in S1)
    return Error::ok();
  }

  Error call_static_main(std::string_view main_class,
                         const std::vector<std::string>& /*args*/) override {
    if (!started_)
      return Error::of(Status::NotStarted, "call_static_main before start()");

    const std::string vm_lib = vm_lib_path();
    if (vm_lib.empty())
      return Error::of(Status::InvalidConfig,
                       "split: set ACL_ART_SPLIT_VMLIB (libjvm.so/libart.so) "
                       "or ACL_ART_HOME");
    const std::string host = art_host_path();
    if (::access(host.c_str(), X_OK) != 0)
      return Error::of(Status::Unavailable,
                       "split: acl-art-host not found/executable at " + host);

    auto [acl_end, child_end] = ipc::Channel::make_pair();
    if (!acl_end.valid())
      return Error::of(Status::RuntimeError, "split: socketpair failed");

    // art-host owns the VM-specific option construction; hand it the
    // classpaths. ART (bionic libart.so): the app DEX + any framework go on the
    // boot classpath (--bcp) because ART's FindClass uses the boot loader;
    // art-host adds the apex core jars + ANDROID_* roots. OpenJDK (x86 dev/CI):
    // the app classes go on --cp. Raw config options pass through as --opt.
    const bool is_art = vm_lib.find("libart") != std::string::npos;
    const std::string app_cp = join(config_.classpath, ':');
    const std::string fw_bcp = join(config_.boot_classpath, ':');
    std::vector<std::string> argv_s = {host, "--ipc-fd=0", "--vm-lib=" + vm_lib,
                                       "--main=" + std::string(main_class)};
    if (is_art) {
      if (const char* h = std::getenv("ACL_ART_HOME"); h && *h)
        argv_s.push_back("--home=" + std::string(h));
      std::string bcp =
          fw_bcp;  // framework (if any) before the app so framework classes win
      if (!app_cp.empty())
        bcp += (bcp.empty() ? "" : ":") + app_cp;
      if (!bcp.empty())
        argv_s.push_back("--bcp=" + bcp);
    } else if (!app_cp.empty()) {
      argv_s.push_back("--cp=" + app_cp);
    }
    for (const auto& o : config_.options)
      argv_s.push_back("--opt=" + o);

    // Child inherits child_end as fd 0 (matches --ipc-fd=0).
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, child_end.native_handle(), 0);

    std::vector<char*> argv;
    for (auto& s : argv_s)
      argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    log::i(kTag, "launching art-host: {} vm-lib={} main={}", host, vm_lib,
           main_class);
    pid_t pid = 0;
    int rc =
        ::posix_spawn(&pid, host.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    child_end.close();  // parent keeps only its end
    if (rc != 0)
      return Error::of(
          Status::RuntimeError,
          "split: posix_spawn failed: " + std::string(std::strerror(rc)));

    // The present sink consumes the dma-buf frames art-host renders (S2). Null
    // until a display backend (Wayland / drm-cxx scanout) is wired; selected
    // via ACL_PRESENT_SINK.
    auto sink = wm::make_present_sink(wm::default_present_backend());

    // Drive the IPC: stream Log output to our stdout, route PresentFrames to
    // the sink, until Bye.
    int child_code = -1;
    ipc::Received r;
    while (acl_end.recv(r)) {
      switch (r.msg.type) {
        case ipc::MsgType::Hello:
          log::i(kTag, "art-host up (proto {})", r.msg.u32);
          break;
        case ipc::MsgType::Log:
          std::fwrite(r.extra.data(), 1, r.extra.size(), stdout);
          std::fflush(stdout);
          break;
        case ipc::MsgType::PresentFrame: {
          const auto& p = r.msg.present;
          wm::ExternalFrame f{.fd = r.fd0,
                              .buffer_id = p.buffer_id,
                              .width = p.w,
                              .height = p.h,
                              .drm_format = p.drm_format,
                              .modifier = p.modifier,
                              .stride = p.stride,
                              .offset = p.offset,
                              .damage_x = p.damage_x,
                              .damage_y = p.damage_y,
                              .damage_w = p.damage_w,
                              .damage_h = p.damage_h,
                              .acquire_fence = r.fd1};
          sink->present(f);  // takes the fds (closes them)
          r.fd0 = r.fd1 = -1;
          break;
        }
        case ipc::MsgType::Bye:
          child_code = static_cast<int>(r.msg.u32);
          break;
        default:
          break;
      }
      if (r.fd0 >= 0)
        ::close(r.fd0);
      if (r.fd1 >= 0)
        ::close(r.fd1);
      if (child_code >= 0)
        break;
    }

    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    if (child_code > 0)
      return Error::of(Status::RuntimeError,
                       "split: art-host reported failure (code " +
                           std::to_string(child_code) + ")");
    return Error::ok();
  }

  Error call_activity_lifecycle(std::string_view) override {
    // S2+ (View render across the dma-buf boundary); fall back to main() for
    // now.
    return Error::of(Status::NotApplicable,
                     "split: activity path is S2 (not yet wired)");
  }
  Error call_media_browser(std::string_view) override {
    return Error::of(Status::NotApplicable, "split: media path not yet wired");
  }
  Error call_car_app(std::string_view) override {
    return Error::of(Status::NotApplicable,
                     "split: car-app path not yet wired");
  }
  Error shutdown() override { return Error::ok(); }

 private:
  VmConfig config_;
  bool started_ = false;
};

}  // namespace

std::unique_ptr<Vm> make_split_vm(VmConfig config) {
  return std::make_unique<SplitVm>(std::move(config));
}

}  // namespace acl::runtime

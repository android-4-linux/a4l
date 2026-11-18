// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test (docs/adr/ADR-0009 C4); the classic BPF
// instruction set per Documentation/networking/filter.rst. No GPL-3.0 sources.
//
// Confinement of the --split app process: the bubblewrap plan and the seccomp
// filter. The filter is run two ways. An interpreter in this file executes
// the generated program against every call number, so the search tree is
// checked exhaustively without a kernel; and a forked child installs it for
// real and makes calls on and off the list. What bubblewrap does with the
// plan is in tests/integration/run_confine_test.py.

#include <catch2/catch_test_macros.hpp>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "confine/confine.h"
#include "confine/seccomp_filter.h"

using namespace acl::confine;

namespace {

#if defined(__x86_64__)
constexpr std::uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
constexpr std::uint32_t kArch = 0;
#endif

constexpr std::uint32_t kBadProgram = 0xdeadbeefu;

// Classic BPF, the subset seccomp accepts and build_filter() emits. A jump
// out of the program or an instruction outside the subset is kBadProgram.
std::uint32_t run(const std::vector<sock_filter>& prog,
                  const seccomp_data& data) {
  std::uint32_t a = 0;
  std::size_t pc = 0;
  for (int steps = 0; steps < 10000; ++steps) {
    if (pc >= prog.size())
      return kBadProgram;
    const sock_filter& i = prog[pc];
    switch (i.code) {
      case BPF_LD | BPF_W | BPF_ABS: {
        if (i.k + 4 > sizeof(data) || i.k % 4 != 0)
          return kBadProgram;
        std::memcpy(&a, reinterpret_cast<const char*>(&data) + i.k, 4);
        ++pc;
        break;
      }
      case BPF_JMP | BPF_JEQ | BPF_K:
        pc += 1 + (a == i.k ? i.jt : i.jf);
        break;
      case BPF_JMP | BPF_JGE | BPF_K:
        pc += 1 + (a >= i.k ? i.jt : i.jf);
        break;
      case BPF_JMP | BPF_JGT | BPF_K:
        pc += 1 + (a > i.k ? i.jt : i.jf);
        break;
      case BPF_JMP | BPF_JSET | BPF_K:
        pc += 1 + ((a & i.k) != 0 ? i.jt : i.jf);
        break;
      case BPF_JMP | BPF_JA:
        pc += 1 + i.k;
        break;
      case BPF_RET | BPF_K:
        return i.k;
      default:
        return kBadProgram;
    }
  }
  return kBadProgram;
}

seccomp_data call(int nr, std::uint64_t a0 = 0, std::uint64_t a1 = 0) {
  seccomp_data d{};
  d.nr = nr;
  d.arch = kArch;
  d.args[0] = a0;
  d.args[1] = a1;
  return d;
}

constexpr std::uint32_t kAllow = SECCOMP_RET_ALLOW;
constexpr std::uint32_t err(int e) {
  return SECCOMP_RET_ERRNO | static_cast<std::uint32_t>(e);
}

}  // namespace

TEST_CASE("confine: path helpers", "[confine]") {
  CHECK(normalize("/a/b/../c//d/") == "/a/c/d");
  CHECK(normalize("/") == "/");
  CHECK(normalize("relative/path").empty());
  CHECK(normalize("").empty());

  CHECK(is_same_or_ancestor("/home/u", "/home/u"));
  CHECK(is_same_or_ancestor("/home/u", "/home/u/x"));
  CHECK(is_same_or_ancestor("/", "/anything"));
  CHECK_FALSE(is_same_or_ancestor("/home/u", "/home/user"));
  CHECK_FALSE(is_same_or_ancestor("/home/u/x", "/home/u"));

  const std::vector<std::string> prot = {"/home/u", "/run/user/1000"};
  CHECK(refuse_reason("/home/u/src/build", prot).empty());
  CHECK(refuse_reason("/usr", prot).empty());
  CHECK(refuse_reason("/mnt/data/aosp/out", prot).empty());
  CHECK_FALSE(refuse_reason("/", prot).empty());
  CHECK_FALSE(refuse_reason("/home", prot).empty());      // top level
  CHECK_FALSE(refuse_reason("/home/u", prot).empty());    // the home itself
  CHECK_FALSE(refuse_reason("/run/user", prot).empty());  // above runtime dir
  CHECK_FALSE(refuse_reason("/mnt", prot).empty());
  CHECK_FALSE(refuse_reason("relative", prot).empty());
}

TEST_CASE("confine: the child's environment is an allowlist", "[confine]") {
  for (const char* name :
       {"ACL_ART_HOME", "ACL_PRESENT_GPU", "ANDROID_ROOT", "LD_LIBRARY_PATH",
        "LANG", "LC_ALL", "VK_DRIVER_FILES", "MESA_LOADER_DRIVER_OVERRIDE",
        "ASAN_OPTIONS", "PATH"}) {
    INFO(name);
    CHECK(env_is_passed(name));
  }
  for (const char* name :
       {"DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY",
        "SSH_AUTH_SOCK", "XDG_RUNTIME_DIR", "GITHUB_TOKEN",
        "AWS_SECRET_ACCESS_KEY", "LD_PRELOAD", "PIPEWIRE_REMOTE",
        // The broker's decisions are not the caller's to make.
        "ACL_APK_VERIFY", "ACL_APK_SIGNER", "ACL_SPLIT_FD",
        "ACL_SPLIT_CONFINED", "ACL_SPLIT_BROKER", "ACL_BWRAP"}) {
    INFO(name);
    CHECK_FALSE(env_is_passed(name));
  }
}

TEST_CASE("confine: binds are minimal and parent-first", "[confine]") {
  const std::vector<std::string> got = minimal_cover(
      {"/opt/b/c", "/opt/a", "/opt/b", "/opt/a", "/opt/ab", "/opt/a/x/y"});
  CHECK(got == std::vector<std::string>{"/opt/a", "/opt/ab", "/opt/b"});
}

TEST_CASE("confine: the bubblewrap command line", "[confine]") {
  Plan plan;
  plan.bwrap = "/usr/bin/bwrap";
  plan.ro = {"/usr", "/opt/art", "/opt/art/lib64"};
  plan.ro_optional = {"/etc/fonts"};
  plan.rw = {"/home/u/.local/share/acl/org.example"};
  plan.devices = {"/dev/dri/renderD128"};
  plan.symlinks = {{"usr/lib64", "/lib64"}};

  const std::vector<std::string> a = bwrap_argv(plan);
  const auto has = [&](std::initializer_list<const char*> seq) {
    const std::vector<std::string> want(seq.begin(), seq.end());
    return std::search(a.begin(), a.end(), want.begin(), want.end()) != a.end();
  };
  const auto pos = [&](const char* flag, const char* arg) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) {
      if (a[i] == flag && a[i + 1] == arg)
        return static_cast<long>(i);
    }
    return -1L;
  };
  REQUIRE(a.front() == "/usr/bin/bwrap");
  REQUIRE(a.back() == "--");
  for (const char* flag : {"--unshare-user", "--unshare-pid", "--unshare-ipc",
                           "--unshare-uts", "--unshare-net", "--disable-userns",
                           "--die-with-parent", "--new-session"}) {
    INFO(flag);
    CHECK(std::find(a.begin(), a.end(), flag) != a.end());
  }
  CHECK(has({"--cap-drop", "ALL"}));
  CHECK(has({"--ro-bind", "/usr", "/usr"}));
  CHECK(has({"--ro-bind", "/opt/art", "/opt/art"}));
  CHECK(pos("--ro-bind", "/opt/art/lib64") < 0);  // covered by its parent
  CHECK(has({"--ro-bind-try", "/etc/fonts", "/etc/fonts"}));
  CHECK(has({"--dev-bind", "/dev/dri/renderD128", "/dev/dri/renderD128"}));
  CHECK(has({"--symlink", "usr/lib64", "/lib64"}));
  CHECK(has({"--remount-ro", "/"}));
  // Nothing of the host is bound whole, and the only writable bind is the
  // app's own directory, after the read-only ones.
  CHECK(pos("--bind", "/") < 0);
  CHECK(pos("--ro-bind", "/") < 0);
  CHECK(pos("--dev-bind", "/dev") < 0);
  CHECK(std::count(a.begin(), a.end(), "--bind") == 1);
  CHECK(pos("--bind", "/home/u/.local/share/acl/org.example") >
        pos("--ro-bind", "/usr"));
  CHECK(pos("--remount-ro", "/") >
        pos("--bind", "/home/u/.local/share/acl/org.example"));

  plan.share_net = true;
  plan.disable_userns = false;
  const std::vector<std::string> b = bwrap_argv(plan);
  CHECK(std::find(b.begin(), b.end(), "--unshare-net") == b.end());
  CHECK(std::find(b.begin(), b.end(), "--disable-userns") == b.end());
  CHECK(std::find(b.begin(), b.end(), "--unshare-user") != b.end());
}

TEST_CASE("seccomp: mode names", "[confine][seccomp]") {
  SeccompMode m{};
  CHECK((parse_seccomp_mode("", m) && m == SeccompMode::Errno));
  CHECK((parse_seccomp_mode("errno", m) && m == SeccompMode::Errno));
  CHECK((parse_seccomp_mode("kill", m) && m == SeccompMode::Kill));
  CHECK((parse_seccomp_mode("off", m) && m == SeccompMode::Off));
  CHECK_FALSE(parse_seccomp_mode("OFF", m));
  CHECK_FALSE(parse_seccomp_mode("log", m));
}

TEST_CASE("seccomp: the filter answers every call number as the list says",
          "[confine][seccomp]") {
  // Three shapes: a few calls, one long run, and enough scattered ranges
  // that the left half of the search tree is longer than a conditional jump
  // can skip (255 instructions), which takes the other encoding.
  std::vector<std::vector<int>> lists = {{0, 1, 3, 60, 231}, {}, {}, {}};
  for (int n = 10; n < 300; ++n)
    lists[1].push_back(n);
  for (int n = 0; n < 1000; n += 3)
    lists[2].push_back(n);
  for (const int nr : lists[2]) {
    if (nr % 2 == 0)
      lists[3].push_back(nr), lists[3].push_back(nr + 1);
  }

  for (const std::vector<int>& list : lists) {
    const std::set<int> allowed(list.begin(), list.end());
    for (const Deny deny : {Deny::Errno, Deny::Kill}) {
      const std::vector<sock_filter> prog = build_filter(list, deny);
      REQUIRE(prog.size() <= BPF_MAXINSNS);
      const std::uint32_t off =
          deny == Deny::Kill ? SECCOMP_RET_KILL_PROCESS : err(ENOSYS);
      int wrong = 0;
      for (int nr = 0; nr < 1200; ++nr) {
#ifdef SYS_clone3
        if (nr == SYS_clone3)
          continue;  // always ENOSYS; checked below
#endif
        // Arguments that pass the three argument checks.
        const std::uint32_t got = run(prog, call(nr, AF_UNIX, TCGETS));
        wrong += got != (allowed.count(nr) ? kAllow : off);
      }
      CHECK(wrong == 0);
    }
  }
  // An empty list allows nothing.
  CHECK(run(build_filter({}, Deny::Errno), call(0)) == err(ENOSYS));
}

TEST_CASE("seccomp: architecture and ABI", "[confine][seccomp]") {
  const std::vector<sock_filter> prog = build_filter({0, 1, 2}, Deny::Errno);
  seccomp_data other = call(1);
  other.arch = AUDIT_ARCH_I386;
  CHECK(run(prog, other) == SECCOMP_RET_KILL_PROCESS);
#if defined(__x86_64__)
  // x32: the same architecture value, bit 30 set in the number.
  CHECK(run(prog, call(1 | 0x40000000)) == err(ENOSYS));
  CHECK(run(build_filter({0, 1, 2}, Deny::Kill), call(1 | 0x40000000)) ==
        SECCOMP_RET_KILL_PROCESS);
#endif
  CHECK(run(prog, call(1)) == kAllow);
}

TEST_CASE("seccomp: argument checks", "[confine][seccomp]") {
  const std::vector<int> list = {
      SYS_read,   SYS_clone, SYS_socket, SYS_ioctl,
#ifdef SYS_clone3
      SYS_clone3,
#endif
  };
  const std::vector<sock_filter> prog = build_filter(list, Deny::Errno);
  const std::vector<sock_filter> strict = build_filter(list, Deny::Kill);

  // clone: a thread and a fork pass; any new namespace does not.
  const std::uint64_t thread =
      CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
      CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
  CHECK(run(prog, call(SYS_clone, thread)) == kAllow);
  CHECK(run(prog, call(SYS_clone, SIGCHLD)) == kAllow);
  for (const std::uint64_t ns :
       {CLONE_NEWUSER, CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWNET, CLONE_NEWIPC,
        CLONE_NEWUTS, CLONE_NEWCGROUP}) {
    INFO(ns);
    CHECK(run(prog, call(SYS_clone, ns | SIGCHLD)) == err(EPERM));
    CHECK(run(prog, call(SYS_clone, thread | ns)) == err(EPERM));
    CHECK(run(strict, call(SYS_clone, ns)) == SECCOMP_RET_KILL_PROCESS);
  }
#ifdef SYS_clone3
  // clone3 is on this list and still answers ENOSYS, in both modes: the
  // C library must be able to fall back.
  CHECK(run(prog, call(SYS_clone3)) == err(ENOSYS));
  CHECK(run(strict, call(SYS_clone3)) == err(ENOSYS));
#endif

  // socket: four families.
  for (const int family : {AF_UNIX, AF_INET, AF_INET6, AF_NETLINK}) {
    INFO(family);
    CHECK(run(prog, call(SYS_socket, static_cast<std::uint64_t>(family))) ==
          kAllow);
  }
  for (const int family :
       {AF_PACKET, AF_ALG, AF_VSOCK, AF_BLUETOOTH, AF_UNSPEC, AF_XDP}) {
    INFO(family);
    CHECK(run(prog, call(SYS_socket, static_cast<std::uint64_t>(family))) ==
          err(EAFNOSUPPORT));
  }

  // ioctl: not the two that write to a terminal's input.
  CHECK(run(prog, call(SYS_ioctl, 0, TIOCSTI)) == err(EPERM));
  CHECK(run(prog, call(SYS_ioctl, 0, TIOCLINUX)) == err(EPERM));
  CHECK(run(prog, call(SYS_ioctl, 0, TCGETS)) == kAllow);
  CHECK(run(prog, call(SYS_ioctl, 0, FIONREAD)) == kAllow);
  // The kernel reads the request as 32 bits; so does the filter.
  CHECK(run(prog, call(SYS_ioctl, 0, 0xffffffff00000000ull | TIOCSTI)) ==
        err(EPERM));

  // A checked call that is not on the list is simply off the list.
  const std::vector<sock_filter> bare = build_filter({SYS_read}, Deny::Errno);
  CHECK(run(bare, call(SYS_clone, SIGCHLD)) == err(ENOSYS));
  CHECK(run(bare, call(SYS_socket, AF_UNIX)) == err(ENOSYS));
  CHECK(run(bare, call(SYS_ioctl, 0, TCGETS)) == err(ENOSYS));
}

#if defined(__x86_64__)
TEST_CASE("seccomp: the recorded list, installed in a real process",
          "[confine][seccomp]") {
  const std::vector<int>& list = allowed_syscalls();
  REQUIRE_FALSE(list.empty());
  const auto on_list = [&](int nr) {
    return std::binary_search(list.begin(), list.end(), nr);
  };
  // What must never be on it, whatever a trace shows.
  for (const int nr : {SYS_ptrace,
                       SYS_process_vm_readv,
                       SYS_process_vm_writev,
                       SYS_mount,
                       SYS_umount2,
                       SYS_pivot_root,
                       SYS_chroot,
                       SYS_unshare,
                       SYS_setns,
                       SYS_bpf,
                       SYS_perf_event_open,
                       SYS_userfaultfd,
                       SYS_keyctl,
                       SYS_add_key,
                       SYS_request_key,
                       SYS_kexec_load,
                       SYS_init_module,
                       SYS_finit_module,
                       SYS_delete_module,
                       SYS_reboot,
                       SYS_swapon,
                       SYS_acct,
                       SYS_settimeofday,
                       SYS_io_uring_setup,
                       SYS_open_by_handle_at,
                       SYS_kcmp,
                       SYS_pidfd_getfd,
                       SYS_move_mount,
                       SYS_fsopen,
                       SYS_iopl,
                       SYS_ioperm,
                       SYS_modify_ldt,
                       SYS_personality,
                       SYS_quotactl}) {
    INFO("system call " << nr);
    CHECK_FALSE(on_list(nr));
  }

  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // In the child: no Catch2, no allocation after the filter is on.
    std::string error;
    if (!install_filter(Deny::Errno, error))
      ::_exit(10);
    if (::syscall(SYS_getpid) <= 0)
      ::_exit(11);  // on the list
    errno = 0;
    if (::syscall(SYS_ptrace, 0 /* PTRACE_TRACEME */, 0, 0, 0) != -1 ||
        errno != ENOSYS)
      ::_exit(12);
    errno = 0;
    if (::syscall(SYS_unshare, CLONE_NEWUSER) != -1 || errno != ENOSYS)
      ::_exit(13);
    errno = 0;
    if (::syscall(SYS_socket, AF_PACKET, SOCK_RAW, 0) != -1 ||
        errno != EAFNOSUPPORT)
      ::_exit(14);
    errno = 0;
    if (::syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0) != -1 ||
        errno != EPERM)
      ::_exit(15);
    errno = 0;
    if (::syscall(SYS_ioctl, 0, TIOCSTI, "x") != -1 || errno != EPERM)
      ::_exit(16);
    const int s =
        static_cast<int>(::syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0));
    if (s < 0)
      ::_exit(17);
    // The filter cannot be taken off again.
    errno = 0;
    if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_STRICT, 0, nullptr) != -1 ||
        errno != ENOSYS)
      ::_exit(18);
    ::_exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("seccomp: kill mode ends the process on a call off the list",
          "[confine][seccomp]") {
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    std::string error;
    if (!install_filter(Deny::Kill, error))
      ::_exit(10);
    ::syscall(SYS_ptrace, 0, 0, 0, 0);
    ::_exit(0);  // not reached
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGSYS);
}
#endif  // __x86_64__

TEST_CASE("confine: only a video decoder's node is passed", "[confine]") {
  using acl::confine::is_video_decoder;
  using acl::confine::VideoNodeFacts;
  // Compressed data in, memory to memory: a decoder.
  CHECK(is_video_decoder({.m2m = true, .coded_input = true}));
  // A camera or a tuner is not memory to memory, whatever it produces.
  CHECK_FALSE(is_video_decoder({.m2m = false, .coded_input = true}));
  CHECK_FALSE(is_video_decoder({}));
  // An encoder and a scaler take raw pictures.
  CHECK_FALSE(is_video_decoder({.m2m = true, .raw_input = true}));
  // A device that takes both is more than a decoder.
  CHECK_FALSE(
      is_video_decoder({.m2m = true, .coded_input = true, .raw_input = true}));
  // A node that listed no formats said nothing to go on.
  CHECK_FALSE(is_video_decoder({.m2m = true}));
}

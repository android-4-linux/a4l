// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (ADR-0009 Decision 2, C4); seccomp(2),
// Documentation/networking/filter.rst, namespaces(7). No GPL-3.0 sources.

#include "confine/seccomp_filter.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include <dirent.h>
#include <linux/audit.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace acl::confine {

namespace {

constexpr std::uint32_t kAllow = SECCOMP_RET_ALLOW;
constexpr std::uint32_t kKill = SECCOMP_RET_KILL_PROCESS;
constexpr std::uint32_t errno_ret(int e) {
  return SECCOMP_RET_ERRNO | (static_cast<std::uint32_t>(e) & SECCOMP_RET_DATA);
}

// Every CLONE_NEW* flag. A confined app gets no namespace of its own: a user
// namespace is where an unprivileged process acquires capabilities.
constexpr std::uint32_t kNamespaceFlags =
    CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC |
    CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET;

constexpr std::uint32_t kOffArch = offsetof(seccomp_data, arch);
constexpr std::uint32_t kOffNr = offsetof(seccomp_data, nr);
// The low 32 bits of argument `i`. Little-endian only, which is every
// architecture this file can be asked for.
constexpr std::uint32_t arg_lo(int i) {
  return static_cast<std::uint32_t>(offsetof(seccomp_data, args)) +
         8u * static_cast<std::uint32_t>(i);
}

sock_filter stmt(std::uint16_t code, std::uint32_t k) {
  return sock_filter{code, 0, 0, k};
}
sock_filter jump(std::uint16_t code,
                 std::uint32_t k,
                 std::uint8_t jt,
                 std::uint8_t jf) {
  return sock_filter{code, jt, jf, k};
}

using Program = std::vector<sock_filter>;
using Range = std::pair<std::uint32_t, std::uint32_t>;  // inclusive

// A binary search over sorted, disjoint ranges of call numbers. The
// accumulator holds the number; every path ends in a return.
Program search(const std::vector<Range>& r,
               std::size_t lo,
               std::size_t hi,
               std::uint32_t deny) {
  Program p;
  if (hi - lo == 1) {
    p.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, r[lo].first, 0, 2));
    p.push_back(jump(BPF_JMP | BPF_JGT | BPF_K, r[lo].second, 1, 0));
    p.push_back(stmt(BPF_RET | BPF_K, kAllow));
    p.push_back(stmt(BPF_RET | BPF_K, deny));
    return p;
  }
  const std::size_t mid = lo + (hi - lo) / 2;
  const Program left = search(r, lo, mid, deny);
  const Program right = search(r, mid, hi, deny);
  if (left.size() <= 255) {
    p.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, r[mid].first,
                     static_cast<std::uint8_t>(left.size()), 0));
  } else {
    p.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, r[mid].first, 0, 1));
    p.push_back(
        stmt(BPF_JMP | BPF_JA, static_cast<std::uint32_t>(left.size())));
  }
  p.insert(p.end(), left.begin(), left.end());
  p.insert(p.end(), right.begin(), right.end());
  return p;
}

// "if the call is `nr`, run `body`", where body ends in a return on every
// path and so never falls through.
void when(Program& p, int nr, const Program& body) {
  p.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(nr), 0,
                   static_cast<std::uint8_t>(body.size())));
  p.insert(p.end(), body.begin(), body.end());
}

bool take(std::vector<int>& list, int nr) {
  const auto it = std::find(list.begin(), list.end(), nr);
  if (it == list.end())
    return false;
  list.erase(it);
  return true;
}

std::string status_field(std::string_view name) {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 &&
        line[name.size()] == ':') {
      const std::size_t v = line.find_first_not_of(" \t", name.size() + 1);
      return v == std::string::npos ? std::string() : line.substr(v);
    }
  }
  return {};
}

}  // namespace

bool parse_seccomp_mode(std::string_view text, SeccompMode& out) {
  if (text.empty() || text == "errno") {
    out = SeccompMode::Errno;
  } else if (text == "kill") {
    out = SeccompMode::Kill;
  } else if (text == "off") {
    out = SeccompMode::Off;
  } else {
    return false;
  }
  return true;
}

const std::vector<int>& allowed_syscalls() {
  static const std::vector<int> kList = [] {
    std::vector<int> v = {
#if defined(__x86_64__)
#define ACL_SYSCALL(name) SYS_##name,
#include "confine/seccomp_allow_x86_64.inc"
#undef ACL_SYSCALL
#endif
    };
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
  }();
  return kList;
}

std::vector<sock_filter> build_filter(const std::vector<int>& allowed,
                                      Deny deny) {
  const std::uint32_t off_list = deny == Deny::Kill ? kKill : errno_ret(ENOSYS);
  const std::uint32_t refused = deny == Deny::Kill ? kKill : errno_ret(EPERM);

  std::vector<int> list;
  for (const int nr : allowed) {
    if (nr >= 0)
      list.push_back(nr);
  }
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());

  Program p;
  // A call made through another architecture's table has other numbers; the
  // list says nothing about it.
  p.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffArch));
#if defined(__x86_64__)
  p.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
#elif defined(__aarch64__)
  p.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0));
#else
  p.push_back(jump(BPF_JMP | BPF_JA, 0, 0, 0));
#endif
  p.push_back(stmt(BPF_RET | BPF_K, kKill));
  p.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffNr));
#if defined(__x86_64__)
  // The x32 ABI shares AUDIT_ARCH_X86_64 and sets this bit in the number.
  p.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
  p.push_back(stmt(BPF_RET | BPF_K, off_list));
#endif

  // clone3 passes its flags in memory, where a filter cannot read them. It
  // always answers ENOSYS, and the C library falls back to clone.
#ifdef SYS_clone3
  take(list, SYS_clone3);
  when(p, SYS_clone3, {stmt(BPF_RET | BPF_K, errno_ret(ENOSYS))});
#endif
#ifdef SYS_clone
  if (take(list, SYS_clone)) {
    when(p, SYS_clone,
         {stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(0)),
          jump(BPF_JMP | BPF_JSET | BPF_K, kNamespaceFlags, 0, 1),
          stmt(BPF_RET | BPF_K, refused), stmt(BPF_RET | BPF_K, kAllow)});
  }
#endif
#ifdef SYS_socket
  if (take(list, SYS_socket)) {
    when(p, SYS_socket,
         {stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(0)),
          jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 4, 0),
          jump(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 3, 0),
          jump(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 2, 0),
          jump(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 1, 0),
          stmt(BPF_RET | BPF_K,
               deny == Deny::Kill ? kKill : errno_ret(EAFNOSUPPORT)),
          stmt(BPF_RET | BPF_K, kAllow)});
  }
#endif
#ifdef SYS_ioctl
  if (take(list, SYS_ioctl)) {
    when(p, SYS_ioctl,
         {stmt(BPF_LD | BPF_W | BPF_ABS, arg_lo(1)),
          jump(BPF_JMP | BPF_JEQ | BPF_K, TIOCSTI, 2, 0),
          jump(BPF_JMP | BPF_JEQ | BPF_K, TIOCLINUX, 1, 0),
          stmt(BPF_RET | BPF_K, kAllow), stmt(BPF_RET | BPF_K, refused)});
  }
#endif

  std::vector<Range> ranges;
  for (const int nr : list) {
    const auto n = static_cast<std::uint32_t>(nr);
    if (!ranges.empty() && ranges.back().second + 1 == n)
      ranges.back().second = n;
    else
      ranges.emplace_back(n, n);
  }
  if (ranges.empty()) {
    p.push_back(stmt(BPF_RET | BPF_K, off_list));
  } else {
    const Program tree = search(ranges, 0, ranges.size(), off_list);
    p.insert(p.end(), tree.begin(), tree.end());
  }
  return p;
}

bool install_filter(Deny deny, std::string& error) {
  const std::vector<int>& allowed = allowed_syscalls();
  if (allowed.empty()) {
    error = "a4l has no system call list for this architecture";
    return false;
  }
  return install_filter(allowed, deny, error);
}

bool install_filter(const std::vector<int>& allowed,
                    Deny deny,
                    std::string& error) {
  std::vector<sock_filter> program = build_filter(allowed, deny);
  if (program.size() > BPF_MAXINSNS) {
    error = "the filter is too long";
    return false;
  }
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    error = std::string("no_new_privs: ") + std::strerror(errno);
    return false;
  }
  sock_fprog prog{};
  prog.len = static_cast<unsigned short>(program.size());
  prog.filter = program.data();
  if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC,
                &prog) != 0) {
    error = std::string("seccomp: ") + std::strerror(errno);
    return false;
  }
  return true;
}

std::string describe_namespaces(bool with_net) {
  std::string out;
  for (const char* ns : {"user", "mnt", "pid", "ipc", "net"}) {
    if (!with_net && std::strcmp(ns, "net") == 0)
      continue;
    char target[64];
    const std::string link = std::string("/proc/self/ns/") + ns;
    const ssize_t n = ::readlink(link.c_str(), target, sizeof(target) - 1);
    if (n <= 0)
      continue;
    target[n] = '\0';
    if (!out.empty())
      out += ' ';
    out += target;
  }
  return out;
}

std::string verify_confined(const SelfCheck& check) {
  // Namespaces: each one the broker named must differ from ours.
  if (check.host_namespaces.empty())
    return "the broker named no namespaces";
  const std::string mine = describe_namespaces(true);
  std::istringstream host(check.host_namespaces);
  std::string id;
  int named = 0;
  while (host >> id) {
    ++named;
    std::istringstream own(mine);
    std::string o;
    while (own >> o) {
      if (o == id)
        return "this process shares the broker's namespace " + id;
    }
  }
  if (named < 4)
    return "the broker named fewer than four namespaces";

  if (status_field("NoNewPrivs") != "1")
    return "no_new_privs is not set";
  if (check.expect_filter && status_field("Seccomp") != "2")
    return "no seccomp filter is installed";

  // Descriptors: standard streams and the broker's socket, nothing else.
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr)
    return "cannot list /proc/self/fd";
  const int scan_fd = ::dirfd(d);
  std::string extra;
  while (const dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    const int fd = std::atoi(e->d_name);
    if (fd <= 2 || fd == check.ipc_fd || fd == scan_fd)
      continue;
    extra = "descriptor " + std::string(e->d_name) +
            " was inherited and should not have been";
    break;
  }
  ::closedir(d);
  return extra;
}

}  // namespace acl::confine

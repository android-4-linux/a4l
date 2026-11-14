// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (ADR-0009 Decision 2, C4); seccomp(2),
// the classic BPF instruction set as documented in
// Documentation/networking/filter.rst. No GPL-3.0 sources.
//
// The seccomp-bpf filter the --split app process installs on itself before
// any byte of the APK runs. It is an allowlist of system call numbers,
// recorded from the app corpus (tools/record-seccomp.sh) and reviewed, with
// three argument checks on calls the list has to contain:
//
//   clone   no new namespace (CLONE_NEWUSER and the other CLONE_NEW* flags)
//   socket  AF_UNIX, AF_INET, AF_INET6 and AF_NETLINK only
//   ioctl   not TIOCSTI, not TIOCLINUX
//
// x86-64 only: the list is per-architecture and has been recorded on one.
#ifndef ACL_CONFINE_SECCOMP_FILTER_H
#define ACL_CONFINE_SECCOMP_FILTER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <linux/filter.h>

namespace acl::confine {

enum class Deny {
  Errno,  // a call off the list fails with ENOSYS, as an unknown call would
  Kill,   // the process is killed with SIGSYS
};

enum class SeccompMode { Off, Errno, Kill };

// "off", "errno" or "kill"; anything else is false. "" is the default, errno.
bool parse_seccomp_mode(std::string_view text, SeccompMode& out);

// The recorded allowlist for this architecture, sorted. Empty where a4l has
// none (anything but x86-64).
const std::vector<int>& allowed_syscalls();

// The filter program for `allowed` (any order, duplicates ignored).
std::vector<sock_filter> build_filter(const std::vector<int>& allowed,
                                      Deny deny);

// Set no_new_privs and install the filter for the recorded list on every
// thread of this process. False and `error` if the architecture has no list
// or the kernel refuses.
bool install_filter(Deny deny, std::string& error);

// The same for a given list, for tests.
bool install_filter(const std::vector<int>& allowed,
                    Deny deny,
                    std::string& error);

// What a confined app process checks about itself before it continues. The
// broker fills `host_namespaces` with its own namespace identities
// (describe_namespaces()); the child must be in different ones.
struct SelfCheck {
  std::string host_namespaces;
  int ipc_fd = -1;  // the one inherited descriptor besides 0, 1, 2
  bool expect_filter = true;
};

// "user:[4026531837] mnt:[...] pid:[...] ipc:[...] net:[...]" for this
// process; `with_net` false leaves the network namespace out, for a child
// that is meant to share it.
std::string describe_namespaces(bool with_net);

// "" if this process is confined as `check` says, else what is wrong: a
// namespace shared with the broker, no_new_privs or the filter missing, or
// a descriptor that should not have been inherited.
std::string verify_confined(const SelfCheck& check);

}  // namespace acl::confine

#endif  // ACL_CONFINE_SECCOMP_FILTER_H

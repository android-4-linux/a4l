// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (ADR-0009 Decision 2, C4); bubblewrap's
// documented command line (bwrap(1)), user_namespaces(7), mount_namespaces(7).
// No GPL-3.0 sources.
//
// Confinement of the --split app process. The broker does not start the app
// process directly: it starts bubblewrap, which builds a user, mount, pid,
// ipc, uts and (by default) network namespace holding a read-only view of
// the host's /usr, the inputs a4l itself needs, one GPU render node and the
// app's own two directories, and then executes a4l inside it. The app process
// installs its seccomp filter itself (confine/seccomp_filter.h) before any
// byte of the APK runs.
//
// This header is the plan: which host paths the child may see, and the
// bubblewrap command line that says so. Building it reads the host filesystem
// and executes nothing.
#ifndef ACL_CONFINE_CONFINE_H
#define ACL_CONFINE_CONFINE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acl::confine {

// What the child sees. Every path is absolute and appears in the sandbox at
// the path it has on the host, so nothing a4l was told by path needs
// translating.
struct Plan {
  std::string bwrap;  // the bubblewrap executable
  // Read-only. Files or directories.
  std::vector<std::string> ro;
  // Read-only, skipped if absent on the host.
  std::vector<std::string> ro_optional;
  // Writable directories: the app's data and cache, nothing else.
  std::vector<std::string> rw;
  // Character devices passed through (GPU render nodes).
  std::vector<std::string> devices;
  // {target, link}: links recreated in the sandbox (/lib64 -> usr/lib64).
  std::vector<std::pair<std::string, std::string>> symlinks;
  bool share_net = false;      // keep the host's network namespace
  bool disable_userns = true;  // bubblewrap >= 0.8: no nested user namespace
  std::uint64_t tmp_bytes = 256ull << 20;  // size cap of the private /tmp
};

// The bubblewrap command line for `plan`, ending in "--"; the program to run
// and its arguments follow. Pure: reads nothing.
std::vector<std::string> bwrap_argv(const Plan& plan);

// What the caller knows that the plan cannot find out by itself.
struct Inputs {
  std::string apk;                    // the admitted APK
  std::string package_dir;            // pkg::safe_package_dir() of the app
  std::vector<std::string> ro_paths;  // --framework, --classpath, ...
  std::vector<std::string> rw_paths;  // --cache
  bool share_net = false;
};

// Result of planning: the plan, and the environment the child gets.
struct Launch {
  Plan plan;
  std::vector<std::string> env;  // "NAME=value", complete
  std::string data_dir;          // the app's writable data directory
  std::string cache_dir;         // and its cache directory
};

// Build the plan for this host and this process's environment. Creates the
// app's data and cache directories (0700) if missing. Returns false and fills
// `error` if confinement cannot be set up: no bubblewrap, no user namespaces,
// or an input that would expose the home or runtime directory.
bool plan_launch(const Inputs& in,
                 char** environment,
                 Launch& out,
                 std::string& error);

// --- pieces, exposed for tests -------------------------------------------

// Lexically normalized absolute path ("" if `p` is not absolute): no ".",
// "..", repeated or trailing slashes. Does not touch the filesystem.
std::string normalize(std::string_view p);

// True if `ancestor` is `path` or a directory above it. Both normalized.
bool is_same_or_ancestor(std::string_view ancestor, std::string_view path);

// Why `path` must not be bound into the sandbox, or "" if it may be: it is
// the root, a top-level directory that is not a system one, or it contains
// one of `protected_dirs` (the home directory, the runtime directory).
std::string refuse_reason(std::string_view path,
                          const std::vector<std::string>& protected_dirs);

// True if the child is given this environment variable. The child's
// environment is an allowlist: a4l's own variables, the loader's, locale,
// GPU driver selection, sanitizer options. Session variables (D-Bus, the
// display, agent sockets) and anything unknown are not passed.
bool env_is_passed(std::string_view name);

// The a4l environment variables whose value is a path, or a colon-separated
// list of paths, that the child reads.
const std::vector<std::string_view>& path_variables();

// Sort, drop duplicates and drop paths already covered by an ancestor in the
// list. Bubblewrap applies binds in order, so a parent must precede a child.
std::vector<std::string> minimal_cover(std::vector<std::string> paths);

}  // namespace acl::confine

#endif  // ACL_CONFINE_CONFINE_H

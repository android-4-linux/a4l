// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design (ADR-0009 Decision 2, C4); bwrap(1),
// user_namespaces(7), the sysfs layout of DRM, V4L2 and media devices as
// documented in Documentation/ABI. No GPL-3.0 sources.

#include "confine/confine.h"

#include "acl_config.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

namespace acl::confine {

namespace fs = std::filesystem;

namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Top-level directories that hold the system, not a user's files.
constexpr std::array<std::string_view, 8> kSystemTop = {
    "/usr", "/lib", "/lib64", "/lib32", "/bin", "/sbin", "/system", "/fonts"};

// Under /etc: what the dynamic loader, the resolver, TLS, fontconfig, the
// GPU loaders and GStreamer's OpenMAX plugin (which decoders the core has)
// read. Nothing else from /etc is visible.
constexpr std::array<std::string_view, 22> kEtc = {"/etc/ld.so.cache",
                                                   "/etc/ld.so.conf",
                                                   "/etc/ld.so.conf.d",
                                                   "/etc/alternatives",
                                                   "/etc/localtime",
                                                   "/etc/nsswitch.conf",
                                                   "/etc/resolv.conf",
                                                   "/etc/hosts",
                                                   "/etc/host.conf",
                                                   "/etc/gai.conf",
                                                   "/etc/ssl",
                                                   "/etc/pki",
                                                   "/etc/ca-certificates",
                                                   "/etc/crypto-policies",
                                                   "/etc/fonts",
                                                   "/etc/glvnd",
                                                   "/etc/vulkan",
                                                   "/etc/drirc",
                                                   "/etc/OpenGL",
                                                   "/etc/libva.conf",
                                                   "/etc/timezone",
                                                   "/etc/xdg/gstomx.conf"};

std::string real(const std::string& p) {
  std::error_code ec;
  const fs::path r = fs::canonical(p, ec);
  return ec ? std::string() : r.string();
}

bool exists(const std::string& p) {
  struct stat st{};
  return ::stat(p.c_str(), &st) == 0;
}

void split_colon(std::string_view v, std::vector<std::string>& out) {
  while (!v.empty()) {
    const std::size_t c = v.find(':');
    const std::string_view part = v.substr(0, c);
    if (!part.empty())
      out.emplace_back(part);
    if (c == std::string_view::npos)
      break;
    v.remove_prefix(c + 1);
  }
}

// Run `argv` and wait. Its output goes nowhere; `out` receives stdout if
// given. Returns the exit status, or -1.
int run_quiet(const std::vector<std::string>& args, std::string* out) {
  int pipefd[2] = {-1, -1};
  if (out != nullptr && ::pipe2(pipefd, O_CLOEXEC) != 0)
    return -1;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  if (out != nullptr)
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
  else
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  std::vector<std::string> copy = args;
  std::vector<char*> argv;
  argv.reserve(copy.size() + 1);
  for (std::string& a : copy)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  char* const envp[] = {nullptr};
  pid_t pid = 0;
  const int rc = ::posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), envp);
  posix_spawn_file_actions_destroy(&fa);
  if (out != nullptr)
    ::close(pipefd[1]);
  if (rc != 0) {
    if (out != nullptr)
      ::close(pipefd[0]);
    return -1;
  }
  if (out != nullptr) {
    char buf[256];
    ssize_t n = 0;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0 && out->size() < 4096)
      out->append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string find_bwrap(char** environment) {
  std::vector<std::string> candidates;
  for (char** e = environment; e != nullptr && *e != nullptr; ++e) {
    const std::string_view v(*e);
    if (starts_with(v, "ACL_BWRAP=") && v.size() > 10)
      return std::string(v.substr(10));  // explicit: used or reported, as is
  }
  candidates = {"/usr/bin/bwrap", "/bin/bwrap", "/usr/local/bin/bwrap"};
  for (const std::string& c : candidates) {
    if (::access(c.c_str(), X_OK) == 0)
      return c;
  }
  return {};
}

// "bubblewrap 0.12.0" -> {0, 12}.
bool bwrap_version(const std::string& bwrap, int& major, int& minor) {
  std::string out;
  if (run_quiet({bwrap, "--version"}, &out) != 0)
    return false;
  const std::size_t sp = out.rfind(' ');
  if (sp == std::string::npos)
    return false;
  char* end = nullptr;
  major = static_cast<int>(std::strtol(out.c_str() + sp + 1, &end, 10));
  if (end == nullptr || *end != '.')
    return false;
  minor = static_cast<int>(std::strtol(end + 1, nullptr, 10));
  return true;
}

bool make_dirs_private(const std::string& dir, std::string& error) {
  std::error_code ec;
  fs::create_directories(fs::path(dir).parent_path(), ec);
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    error = "cannot create " + dir + ": " + std::strerror(errno);
    return false;
  }
  struct stat st{};
  if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::geteuid()) {
    error = dir + " is not a directory owned by this user";
    return false;
  }
  return true;
}

std::string xdg_dir(char** environment,
                    std::string_view var,
                    std::string_view home,
                    std::string_view fallback) {
  const std::string prefix = std::string(var) + "=";
  for (char** e = environment; e != nullptr && *e != nullptr; ++e) {
    const std::string_view v(*e);
    if (starts_with(v, prefix) && v.size() > prefix.size() &&
        v[prefix.size()] == '/')
      return normalize(v.substr(prefix.size()));
  }
  if (home.empty())
    return {};
  return std::string(home) + "/" + std::string(fallback);
}

std::string env_value(char** environment, std::string_view name) {
  for (char** e = environment; e != nullptr && *e != nullptr; ++e) {
    const std::string_view v(*e);
    if (v.size() > name.size() && v[name.size()] == '=' && starts_with(v, name))
      return std::string(v.substr(name.size() + 1));
  }
  return {};
}

// Files this process has mapped that live outside the system directories:
// the a4l binary itself and, in a build tree, its own shared libraries.
void own_mappings(std::vector<std::string>& out) {
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    const std::size_t slash = line.find('/');
    if (slash == std::string::npos)
      continue;
    std::string path = line.substr(slash);
    static constexpr std::string_view kDeleted = " (deleted)";
    if (path.size() > kDeleted.size() &&
        path.compare(path.size() - kDeleted.size(), kDeleted.size(),
                     kDeleted) == 0)
      continue;
    if (starts_with(path, "/usr/") || starts_with(path, "/lib") ||
        starts_with(path, "/dev/") || starts_with(path, "/memfd:"))
      continue;
    out.push_back(std::move(path));
  }
}

// The sysfs entries a device node is looked up by, as udev (and so
// GStreamer) does: the device's directory, read-only; the
// /sys/dev/char/<major>:<minor> link that finds it by number; and its entry
// in `list`, the directory its subsystem's devices are listed in. Each link
// leads to that one directory and nothing else. `up` is how many levels the
// device's directory is above the node's own sysfs directory: 2 under a
// class directory (.../<device>/drm/renderD128), 1 on a bus
// (.../<device>/media0).
void sysfs_entries(Plan& plan,
                   const std::string& node,
                   const std::string& list,
                   int up) {
  struct stat st{};
  if (::stat(node.c_str(), &st) != 0 || !S_ISCHR(st.st_mode))
    return;
  const std::string link = "/sys/dev/char/" +
                           std::to_string(major(st.st_rdev)) + ":" +
                           std::to_string(minor(st.st_rdev));
  char target[512];
  const ssize_t n = ::readlink(link.c_str(), target, sizeof(target) - 1);
  const std::string resolved = real(link);  // /sys/devices/.../<node>
  if (n <= 0 || resolved.empty() || !starts_with(resolved, "/sys/devices/"))
    return;
  target[n] = '\0';
  fs::path device(resolved);
  for (int i = 0; i < up; ++i)
    device = device.parent_path();
  if (!starts_with(device.string(), "/sys/devices/"))
    return;
  if (std::find(plan.ro.begin(), plan.ro.end(), device.string()) ==
      plan.ro.end())
    plan.ro.push_back(device.string());
  plan.symlinks.emplace_back(target, link);
  const std::string name = fs::path(resolved).filename().string();
  plan.symlinks.emplace_back(
      fs::path(resolved).lexically_relative(list).string(), list + "/" + name);
}

// The GPU render nodes, and for each its sysfs entries, which the driver
// loaders read to identify it. GStreamer's VA plugin asks udev for the DRM
// devices and registers no decoder when it is told there are none.
void gpu_nodes(Plan& plan) {
  DIR* d = ::opendir("/dev/dri");
  if (d == nullptr)
    return;
  while (const dirent* e = ::readdir(d)) {
    if (std::strncmp(e->d_name, "renderD", 7) != 0)
      continue;
    const std::string node = std::string("/dev/dri/") + e->d_name;
    struct stat st{};
    if (::stat(node.c_str(), &st) != 0 || !S_ISCHR(st.st_mode))
      continue;
    plan.devices.push_back(node);
    sysfs_entries(plan, node, "/sys/class/drm", 2);
  }
  ::closedir(d);
  std::sort(plan.devices.begin(), plan.devices.end());
}

bool is_char_device(const char* path) {
  struct stat st{};
  return ::stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

// A GPU whose kernel driver is not a DRM one has no render node. Qualcomm's
// downstream Adreno driver (kgsl) is the one known here: its user space opens
// /dev/kgsl-3d0 and takes every buffer from the ion allocator, and the
// vendor's video decoder allocates there as well. Ion is passed only beside
// that GPU.
void vendor_gpu_nodes(Plan& plan) {
  if (!is_char_device("/dev/kgsl-3d0"))
    return;
  plan.devices.emplace_back("/dev/kgsl-3d0");
  if (is_char_device("/dev/ion"))
    plan.devices.emplace_back("/dev/ion");
  // The driver's gbm will not start without a /dev/dri/card* it can open:
  // it lists the directory, opens the first one and makes no call on it. A
  // card node is the display controller, which an app has no business with,
  // so the name is given and the node is not: a link to the render node,
  // which the app has already.
  const std::string render = "/dev/dri/renderD";
  for (const std::string& d : plan.devices) {
    if (d.compare(0, render.size(), render) == 0) {
      plan.symlinks.emplace_back(d.substr(d.rfind('/') + 1), "/dev/dri/card0");
      break;
    }
  }
  std::sort(plan.devices.begin(), plan.devices.end());
}

int xioctl(int fd, unsigned long request, void* arg) {
  int r;
  do {
    r = ::ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

// The numbered nodes /dev/<prefix>N, in order.
std::vector<std::string> numbered_nodes(const char* prefix) {
  std::vector<int> numbers;
  const std::size_t len = std::strlen(prefix);
  if (DIR* d = ::opendir("/dev")) {
    while (const dirent* e = ::readdir(d)) {
      if (std::strncmp(e->d_name, prefix, len) != 0)
        continue;
      char* end = nullptr;
      const long n = std::strtol(e->d_name + len, &end, 10);
      if (end != e->d_name + len && *end == '\0' && n >= 0 && n < 4096)
        numbers.push_back(static_cast<int>(n));
    }
    ::closedir(d);
  }
  std::sort(numbers.begin(), numbers.end());
  std::vector<std::string> out;
  out.reserve(numbers.size());
  for (const int n : numbers)
    out.push_back(std::string("/dev/") + prefix + std::to_string(n));
  return out;
}

// Hardware video decoders. A decoder that is its own device (V4L2, on Arm
// boards) is reached by its node, as the GPU is; without it the app process
// decodes in software or not at all. Each /dev/video* is asked what it is,
// and only a decoder is passed: the same directory holds cameras. A decoder
// driven by requests is also reached through its media controller, found by
// the driver's name. Each has its sysfs entries: GStreamer's V4L2 stateless
// plugin finds a decoder by listing the media bus and following the media
// controller to its video node, and registers none when udev shows neither.
void video_decoder_nodes(Plan& plan) {
  std::vector<std::string> drivers;
  for (const std::string& node : numbered_nodes("video")) {
    const int fd = ::open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;
    VideoNodeFacts facts;
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
      const std::uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                                     ? cap.device_caps
                                     : cap.capabilities;
      // A driver older than the M2M capability says it has both queues.
      constexpr std::uint32_t kBothMplane =
          V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE;
      constexpr std::uint32_t kBoth =
          V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT;
      std::uint32_t input = 0, output = 0;
      if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0 ||
          (caps & kBothMplane) == kBothMplane) {
        input = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        output = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      } else if ((caps & V4L2_CAP_VIDEO_M2M) != 0 || (caps & kBoth) == kBoth) {
        input = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        output = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      }
      facts.m2m = input != 0;
      for (std::uint32_t i = 0; facts.m2m && i < 64; ++i) {
        v4l2_fmtdesc desc{};
        desc.index = i;
        desc.type = input;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0)
          break;
        if ((desc.flags & V4L2_FMT_FLAG_COMPRESSED) != 0)
          facts.coded_input = true;
        else
          facts.raw_input = true;
      }
      for (std::uint32_t i = 0; facts.m2m && i < 64; ++i) {
        v4l2_fmtdesc desc{};
        desc.index = i;
        desc.type = output;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0)
          break;
        if ((desc.flags & V4L2_FMT_FLAG_COMPRESSED) != 0)
          facts.coded_output = true;
      }
    }
    ::close(fd);
    if (!is_video_decoder(facts))
      continue;
    plan.devices.push_back(node);
    sysfs_entries(plan, node, "/sys/class/video4linux", 2);
    cap.driver[sizeof(cap.driver) - 1] = 0;
    drivers.emplace_back(reinterpret_cast<const char*>(cap.driver));
  }
  if (drivers.empty())
    return;
  for (const std::string& node : numbered_nodes("media")) {
    const int fd = ::open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;
    media_device_info info{};
    const bool known = xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0;
    ::close(fd);
    info.driver[sizeof(info.driver) - 1] = 0;
    if (known && std::find(drivers.begin(), drivers.end(), info.driver) !=
                     drivers.end()) {
      plan.devices.push_back(node);
      sysfs_entries(plan, node, "/sys/bus/media/devices", 1);
    }
  }
  std::sort(plan.devices.begin(), plan.devices.end());
}

}  // namespace

bool is_video_decoder(const VideoNodeFacts& facts) {
  return facts.m2m && facts.coded_input && !facts.raw_input &&
         !facts.coded_output;
}

std::string normalize(std::string_view p) {
  if (p.empty() || p[0] != '/')
    return {};
  std::string n = fs::path(p).lexically_normal().string();
  if (n.size() > 1 && n.back() == '/')
    return n.substr(0, n.size() - 1);
  return n;
}

bool is_same_or_ancestor(std::string_view ancestor, std::string_view path) {
  if (ancestor.empty() || path.empty())
    return false;
  if (ancestor == "/")
    return true;
  if (!starts_with(path, ancestor))
    return false;
  return path.size() == ancestor.size() || path[ancestor.size()] == '/';
}

std::string refuse_reason(std::string_view path,
                          const std::vector<std::string>& protected_dirs) {
  if (path.empty() || path[0] != '/')
    return "it is not an absolute path";
  if (path == "/")
    return "it is the root directory";
  if (path.find('/', 1) == std::string_view::npos &&
      std::find(kSystemTop.begin(), kSystemTop.end(), path) == kSystemTop.end())
    return "it is a top-level directory";
  for (const std::string& dir : protected_dirs) {
    if (is_same_or_ancestor(path, dir))
      return "it contains " + dir;
  }
  return {};
}

bool env_is_passed(std::string_view name) {
  static constexpr std::array<std::string_view, 22> kExact = {
      "PATH",
      "HOME",
      "LANG",
      "LANGUAGE",
      "TZ",
      "LD_LIBRARY_PATH",
      "DRI_PRIME",
      "ASAN_OPTIONS",
      "UBSAN_OPTIONS",
      "LSAN_OPTIONS",
      "TSAN_OPTIONS",
      "MSAN_OPTIONS",
      "HTTP_PROXY",
      "HTTPS_PROXY",
      "NO_PROXY",
      "http_proxy",
      "https_proxy",
      "no_proxy",
      "NO_COLOR",
      "TERM",
      "MALLOC_PERTURB_",
      "MALLOC_ARENA_MAX"};
  static constexpr std::array<std::string_view, 14> kPrefix = {
      "ACL_",     "ANDROID_", "LC_",  "VK_",    "MESA_", "LIBGL_", "EGL_",
      "GALLIUM_", "RADV_",    "ANV_", "INTEL_", "AMD_",  "__GL",   "LIBVA_"};
  // Decided by the broker for the child, never taken from the caller.
  static constexpr std::array<std::string_view, 7> kNever = {
      "ACL_APK_VERIFY",    "ACL_APK_SIGNER",   "ACL_SPLIT_FD",
      "ACL_SPLIT_DISPLAY", "ACL_SPLIT_BROKER", "ACL_SPLIT_CONFINED",
      "ACL_BWRAP"};
  if (std::find(kNever.begin(), kNever.end(), name) != kNever.end())
    return false;
  if (std::find(kExact.begin(), kExact.end(), name) != kExact.end())
    return true;
  for (const std::string_view p : kPrefix) {
    if (starts_with(name, p))
      return true;
  }
  return false;
}

const std::vector<std::string_view>& path_variables() {
  static const std::vector<std::string_view> kVars = {
      "ACL_ART_HOME",        "ACL_ART_BOOTCLASSPATH", "ACL_ART_HOST_BOOTSTRAP",
      "ACL_ART_LIBPATH",     "ACL_ART_FRAMEWORK_RES", "ACL_ART_GRAPHICS_LIB",
      "ACL_ART_GLES_LIB",    "ACL_ART_FRAMEWORK",     "ACL_ART_HOST",
      "ACL_ART_SPLIT_VMLIB", "ACL_FRAMEWORK",         "ACL_ANDROID_JAR",
      "ACL_BIONIC_LINKER",   "ACL_BIONIC_SYSROOT",    "ACL_BIONIC_STUB",
      "ACL_BIONIC_APP_LIBS", "ACL_SPLIT_APKS",        "ACL_GRAPHICS_FONT",
      "ACL_FONT_SERIF",      "ACL_FONT_MONO",         "ACL_WEBVIEW_APK",
      "ACL_SPLIT_RO",        "LD_LIBRARY_PATH"};
  return kVars;
}

std::vector<std::string> minimal_cover(std::vector<std::string> paths) {
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  std::vector<std::string> out;
  for (std::string& p : paths) {
    bool covered = false;
    for (const std::string& kept : out)
      covered = covered || is_same_or_ancestor(kept, p);
    if (!covered)
      out.push_back(std::move(p));
  }
  return out;
}

std::vector<std::string> bwrap_argv(const Plan& plan) {
  std::vector<std::string> a = {plan.bwrap,      "--unshare-user",
                                "--unshare-pid", "--unshare-ipc",
                                "--unshare-uts", "--unshare-cgroup-try"};
  if (!plan.share_net)
    a.emplace_back("--unshare-net");
  if (plan.disable_userns)
    a.emplace_back("--disable-userns");
  a.insert(a.end(), {"--die-with-parent", "--new-session", "--cap-drop", "ALL",
                     "--proc", "/proc", "--dev", "/dev", "--size",
                     std::to_string(plan.tmp_bytes), "--tmpfs", "/tmp"});
  for (const std::string& d : plan.devices)
    a.insert(a.end(), {"--dev-bind", d, d});
  for (const auto& [target, link] : plan.symlinks)
    a.insert(a.end(), {"--symlink", target, link});
  for (const std::string& p : minimal_cover(plan.ro))
    a.insert(a.end(), {"--ro-bind", p, p});
  for (const std::string& p : minimal_cover(plan.ro_optional))
    a.insert(a.end(), {"--ro-bind-try", p, p});
  // Writable last, so that a writable directory below a read-only one stays
  // writable.
  for (const std::string& p : minimal_cover(plan.rw))
    a.insert(a.end(), {"--bind", p, p});
  a.insert(a.end(), {"--remount-ro", "/", "--chdir", "/", "--"});
  return a;
}

void spawn_close_from(posix_spawn_file_actions_t* fa, int first) {
#if ACL_HAVE_SPAWN_CLOSEFROM
  posix_spawn_file_actions_addclosefrom_np(fa, first);
#else
  std::error_code ec;
  for (const auto& e : fs::directory_iterator("/proc/self/fd", ec)) {
    const std::string name = e.path().filename().string();
    char* end = nullptr;
    const long fd = std::strtol(name.c_str(), &end, 10);
    if (end != name.c_str() && *end == '\0' && fd >= first)
      posix_spawn_file_actions_addclose(fa, static_cast<int>(fd));
  }
#endif
}

int run_in_sandbox(const Plan& plan,
                   const std::vector<std::string>& program,
                   const std::vector<std::string>& env,
                   int timeout_ms) {
  std::vector<std::string> args = bwrap_argv(plan);
  args.insert(args.end(), program.begin(), program.end());
  std::vector<std::string> env_copy = env;
  std::vector<char*> argv, envp;
  for (std::string& a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  for (std::string& e : env_copy)
    envp.push_back(e.data());
  envp.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  spawn_close_from(&fa, 3);
  pid_t pid = 0;
  const int rc =
      ::posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0)
    return -1;
  // Bubblewrap ends its child when it is killed (--die-with-parent).
  int status = 0;
  for (int waited = 0;; waited += 5) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid)
      break;
    if (r < 0 && errno != EINTR)
      return -1;
    if (waited >= timeout_ms) {
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      return -1;
    }
    const timespec ts{0, 5'000'000};
    ::nanosleep(&ts, nullptr);
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool plan_launch(const Inputs& in,
                 char** environment,
                 Launch& out,
                 std::string& error) {
  Plan& plan = out.plan;
  plan.bwrap = find_bwrap(environment);
  if (plan.bwrap.empty() || ::access(plan.bwrap.c_str(), X_OK) != 0) {
    error = plan.bwrap.empty()
                ? "bubblewrap (bwrap) is not installed"
                : "$ACL_BWRAP '" + plan.bwrap + "' is not executable";
    return false;
  }
  int major = 0, minor = 0;
  if (!bwrap_version(plan.bwrap, major, minor)) {
    error = plan.bwrap + " --version did not answer";
    return false;
  }
  plan.disable_userns = major > 0 || minor >= 8;
  plan.share_net = in.share_net;
  // A dry run of the namespaces alone. Where unprivileged user namespaces
  // are switched off this is what fails, and it says so better than a child
  // that never starts.
  {
    std::vector<std::string> probe = {plan.bwrap,      "--unshare-user",
                                      "--unshare-pid", "--unshare-ipc",
                                      "--unshare-uts", "--unshare-net"};
    if (plan.disable_userns)
      probe.emplace_back("--disable-userns");
    // The whole host, read-only: this run executes bubblewrap itself and
    // nothing else, and only the namespaces are in question.
    probe.insert(probe.end(),
                 {"--ro-bind", "/", "/", "--", plan.bwrap, "--version"});
    if (run_quiet(probe, nullptr) != 0) {
      error =
          "bubblewrap cannot create a user namespace on this host "
          "(unprivileged user namespaces are disabled or restricted)";
      return false;
    }
  }

  const std::string home = normalize(env_value(environment, "HOME"));
  std::vector<std::string> protected_dirs;
  if (!home.empty() && home != "/") {
    protected_dirs.push_back(home);
    if (const std::string r = real(home); !r.empty() && r != home)
      protected_dirs.push_back(r);
  }
  if (const std::string rt =
          normalize(env_value(environment, "XDG_RUNTIME_DIR"));
      !rt.empty())
    protected_dirs.push_back(rt);

  // The system, read-only.
  for (const std::string_view top : kSystemTop) {
    const std::string p(top);
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0)
      continue;
    if (S_ISLNK(st.st_mode)) {
      char target[256];
      const ssize_t n = ::readlink(p.c_str(), target, sizeof(target) - 1);
      if (n > 0) {
        target[n] = '\0';
        plan.symlinks.emplace_back(target, p);
      }
    } else if (S_ISDIR(st.st_mode)) {
      plan.ro.push_back(p);
    }
  }
  for (const std::string_view e : kEtc)
    plan.ro_optional.emplace_back(e);
  // How many processors there are and how they are laid out. The C library
  // reads it for sysconf(), and a library that cannot may give up when it is
  // loaded: with it hidden, a GStreamer plugin (ONNX Runtime inside it)
  // aborted the app process on a Pi 5 while the plugins were scanned.
  plan.ro_optional.emplace_back("/sys/devices/system/cpu");
  gpu_nodes(plan);
  vendor_gpu_nodes(plan);
  video_decoder_nodes(plan);

  // a4l's own inputs: what the caller named, what the environment names, and
  // the binary. Each is bound where it is and, if that is a symbolic link,
  // where it leads as well.
  std::vector<std::string> wanted = in.ro_paths;
  wanted.push_back(in.apk);
  own_mappings(wanted);
  for (const std::string_view var : path_variables()) {
    const std::string v = env_value(environment, var);
    if (!v.empty())
      split_colon(v, wanted);
  }
  if (const std::string image = env_value(environment, "ACL_ART_BOOT_IMAGE");
      !image.empty())
    wanted.push_back(fs::path(image).parent_path().string());
  for (const std::string& w : wanted) {
    const std::string n = normalize(w);
    if (n.empty() || !exists(n))
      continue;  // a relative or absent path: the child would not find it
    for (const std::string& p : {n, real(n)}) {
      if (p.empty())
        continue;
      if (const std::string why = refuse_reason(p, protected_dirs);
          !why.empty()) {
        error = "refusing to show " + p + " to the app process: " + why;
        return false;
      }
      plan.ro.push_back(p);
    }
  }

  // The app's own directories, the only writable ones.
  const std::string data_home =
      xdg_dir(environment, "XDG_DATA_HOME", home, ".local/share");
  const std::string cache_home =
      xdg_dir(environment, "XDG_CACHE_HOME", home, ".cache");
  if (data_home.empty() || cache_home.empty() || in.package_dir.empty()) {
    error = "no home directory to keep the app's data in";
    return false;
  }
  out.data_dir = data_home + "/acl/" + in.package_dir;
  out.cache_dir = cache_home + "/acl/" + in.package_dir;
  for (const std::string& d :
       {out.data_dir, out.cache_dir, out.cache_dir + "/android-data",
        out.cache_dir + "/mesa"}) {
    if (!make_dirs_private(d, error))
      return false;
  }
  plan.rw = {out.data_dir, out.cache_dir};
  for (const std::string& w : in.rw_paths) {
    const std::string n = normalize(w);
    if (n.empty())
      continue;
    if (const std::string why = refuse_reason(n, protected_dirs);
        !why.empty()) {
      error = "refusing to give " + n + " to the app process: " + why;
      return false;
    }
    if (!make_dirs_private(n, error))
      return false;
    plan.rw.push_back(n);
  }

  // The child's environment: an allowlist, then what the sandbox decides.
  static constexpr std::array<std::string_view, 6> kSet = {
      "XDG_DATA_HOME", "XDG_CACHE_HOME",        "ANDROID_DATA",
      "TMPDIR",        "MESA_SHADER_CACHE_DIR", "XDG_RUNTIME_DIR"};
  for (char** e = environment; e != nullptr && *e != nullptr; ++e) {
    const std::string_view v(*e);
    const std::size_t eq = v.find('=');
    if (eq == std::string_view::npos || eq == 0)
      continue;
    const std::string_view name = v.substr(0, eq);
    if (!env_is_passed(name) ||
        std::find(kSet.begin(), kSet.end(), name) != kSet.end())
      continue;
    out.env.emplace_back(v);
  }
  out.env.push_back("XDG_DATA_HOME=" + data_home);
  out.env.push_back("XDG_CACHE_HOME=" + cache_home);
  out.env.push_back("ANDROID_DATA=" + out.cache_dir + "/android-data");
  out.env.push_back("MESA_SHADER_CACHE_DIR=" + out.cache_dir + "/mesa");
  out.env.emplace_back("TMPDIR=/tmp");
  return true;
}

}  // namespace acl::confine

// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l code; gpsd's client protocol as gpsd_json(5)
// documents it (the ?WATCH command and the stream of JSON lines that follows);
// the sockets API of POSIX. No gpsd code, and no libgps: its ABI changes
// between gpsd releases, and the protocol does not. No GPL-3.0 sources.
#include "svc/location/gpsd_source.h"

#include <netdb.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "log/log.h"
#include "svc/location/gpsd_tpv.h"

namespace acl::svc::location {
namespace {
constexpr const char* kTag = "acl.svc.location";

// A line longer than this is not gpsd's: its longest, a SKY report of every
// satellite in view, is a few kilobytes.
constexpr std::size_t kMaxLine = 64 * 1024;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kFirstRetryMs = 1000;
constexpr int kLastRetryMs = 30000;

// The one thread that talks to gpsd, for the life of the process once a gpsd
// source has started. A source that stops parks it and one that starts wakes
// it: the thread delivers fixes into the Java runtime, which a thread that
// has done so must not exit under, and the hub that stops a source holds a
// lock the delivery takes, so stopping never waits for the thread.
class GpsdWorker {
 public:
  static GpsdWorker& instance() {
    static GpsdWorker* worker = new GpsdWorker;  // never destroyed
    return *worker;
  }

  void activate(std::string host, std::string port, LocationSink sink) {
    {
      const std::lock_guard<std::mutex> lock(m_);
      host_ = std::move(host);
      port_ = std::move(port);
      sink_ = std::move(sink);
      active_ = true;
      ++generation_;
      if (wake_fd_ < 0)
        wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
      if (!thread_started_) {
        thread_started_ = true;
        std::thread([this] { run(); }).detach();
      }
    }
    wake();
  }

  void deactivate() {
    {
      const std::lock_guard<std::mutex> lock(m_);
      active_ = false;
      sink_ = nullptr;
      ++generation_;
    }
    wake();
  }

 private:
  struct Job {
    std::uint64_t generation = 0;
    bool active = false;
    std::string host, port;
    LocationSink sink;
  };

  Job job() {
    const std::lock_guard<std::mutex> lock(m_);
    return Job{generation_, active_, host_, port_, sink_};
  }
  bool current(const Job& j) {
    const std::lock_guard<std::mutex> lock(m_);
    return j.generation == generation_;
  }
  void wake() const {
    const std::uint64_t one = 1;
    if (wake_fd_ >= 0)
      (void)!::write(wake_fd_, &one, sizeof(one));
  }
  // Waits for the job to change, for at most `ms` (forever if negative).
  void wait(int ms) const {
    struct pollfd p{wake_fd_, POLLIN, 0};
    if (::poll(&p, 1, ms) > 0) {
      std::uint64_t n = 0;
      (void)!::read(wake_fd_, &n, sizeof(n));
    }
  }

  // A connection to gpsd, or -1. Gives up when the job changes.
  int connect(const Job& j, std::string* why) {
    struct addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* list = nullptr;
    const int rc = ::getaddrinfo(j.host.c_str(), j.port.c_str(), &hints, &list);
    if (rc != 0) {
      *why = ::gai_strerror(rc);
      return -1;
    }
    int fd = -1;
    for (const struct addrinfo* a = list; a != nullptr && fd < 0;
         a = a->ai_next) {
      const int s =
          ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                   a->ai_protocol);
      if (s < 0) {
        *why = std::strerror(errno);
        continue;
      }
      int err = 0;
      if (::connect(s, a->ai_addr, a->ai_addrlen) != 0) {
        err = errno;
        if (err == EINPROGRESS) {
          struct pollfd p[2] = {{s, POLLOUT, 0}, {wake_fd_, POLLIN, 0}};
          const int n = ::poll(p, 2, kConnectTimeoutMs);
          socklen_t len = sizeof(err);
          if (n <= 0 || (p[0].revents & (POLLOUT | POLLERR | POLLHUP)) == 0)
            err = n == 0 ? ETIMEDOUT : EINTR;
          else if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
            err = errno;
        }
      }
      if (err == 0) {
        fd = s;
      } else {
        *why = std::strerror(err);
        ::close(s);
        if (!current(j))
          break;
      }
    }
    ::freeaddrinfo(list);
    return fd;
  }

  // Reads gpsd's lines and delivers the fixes among them until the
  // connection ends or the job changes.
  void watch(int fd, const Job& j) {
    static constexpr std::string_view kWatch =
        "?WATCH={\"enable\":true,\"json\":true};\n";
    std::size_t sent = 0;
    while (sent < kWatch.size()) {
      const ssize_t n =
          ::send(fd, kWatch.data() + sent, kWatch.size() - sent, MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<std::size_t>(n);
      } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        struct pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, kConnectTimeoutMs) <= 0)
          return;
      } else {
        return;
      }
    }
    std::string pending;
    bool told_waiting = false, told_fix = false;
    char buf[4096];
    for (;;) {
      struct pollfd p[2] = {{fd, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
      if (::poll(p, 2, -1) < 0 && errno != EINTR)
        return;
      if ((p[1].revents & POLLIN) != 0) {
        std::uint64_t n = 0;
        (void)!::read(wake_fd_, &n, sizeof(n));
        if (!current(j))
          return;
      }
      if ((p[0].revents & (POLLIN | POLLERR | POLLHUP)) == 0)
        continue;
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
        return;  // gpsd closed the connection, or it broke
      if (n < 0)
        continue;
      pending.append(buf, static_cast<std::size_t>(n));
      std::size_t start = 0;
      for (std::size_t nl = pending.find('\n', start); nl != std::string::npos;
           nl = pending.find('\n', start)) {
        const std::string_view line(pending.data() + start, nl - start);
        start = nl + 1;
        LocationFix fix{};
        int mode = -1;
        if (fix_from_gpsd_line(line, &fix, &mode)) {
          if (!told_fix) {
            told_fix = true;
            log::i(kTag, "gpsd: the receiver has a fix ({})",
                   mode == 3 ? "3D" : "2D");
          }
          if (j.sink)
            j.sink(fix);
        } else if (mode >= 0 && !told_waiting && !told_fix) {
          told_waiting = true;
          log::i(kTag, "gpsd: the receiver has no fix yet");
        }
      }
      pending.erase(0, start);
      if (pending.size() > kMaxLine) {
        log::w(kTag, "gpsd: a line of over {} bytes; reconnecting", kMaxLine);
        return;
      }
    }
  }

  void run() {
    int retry_ms = kFirstRetryMs;
    bool told_unreachable = false;
    std::uint64_t told_for = 0;
    for (;;) {
      const Job j = job();
      if (!j.active) {
        wait(-1);
        continue;
      }
      if (told_for != j.generation) {
        told_for = j.generation;
        told_unreachable = false;
        retry_ms = kFirstRetryMs;
      }
      std::string why;
      const int fd = connect(j, &why);
      if (fd < 0) {
        if (!told_unreachable && current(j)) {
          told_unreachable = true;
          log::w(kTag,
                 "gpsd at {}:{} is not reachable ({}); trying again until it "
                 "is",
                 j.host, j.port, why);
        }
        if (current(j))
          wait(retry_ms);
        retry_ms = std::min(retry_ms * 2, kLastRetryMs);
        continue;
      }
      log::i(kTag, "gpsd: connected to {}:{}", j.host, j.port);
      told_unreachable = false;
      retry_ms = kFirstRetryMs;
      watch(fd, j);
      ::close(fd);
      if (current(j)) {
        log::i(kTag, "gpsd: the connection ended; reconnecting");
        wait(kFirstRetryMs);
      }
    }
  }

  std::mutex m_;
  bool active_ = false;
  bool thread_started_ = false;
  std::uint64_t generation_ = 0;
  std::string host_, port_;
  LocationSink sink_;
  int wake_fd_ = -1;
};

class GpsdSource final : public LocationSource {
 public:
  GpsdSource(std::string host, std::string port)
      : host_(std::move(host)), port_(std::move(port)) {}
  ~GpsdSource() override { stop(); }
  const char* name() const override { return "gpsd"; }
  bool start(LocationSink sink) override {
    started_ = true;
    GpsdWorker::instance().activate(host_, port_, std::move(sink));
    return true;  // the provider is there, with or without a fix
  }
  void stop() override {
    if (started_) {
      started_ = false;
      GpsdWorker::instance().deactivate();
    }
  }

 private:
  std::string host_, port_;
  bool started_ = false;
};

}  // namespace

std::unique_ptr<LocationSource> make_gpsd_source(std::string host,
                                                 std::string port) {
  return std::make_unique<GpsdSource>(std::move(host), std::move(port));
}

}  // namespace acl::svc::location

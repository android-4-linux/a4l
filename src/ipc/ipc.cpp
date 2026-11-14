// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md). SOCK_SEQPACKET +
// SCM_RIGHTS fd passing. Standard POSIX only (works under both glibc and
// bionic). No GPL-3.0 sources.
#include "ipc/ipc.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>

namespace acl::ipc {

Channel& Channel::operator=(Channel&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    send_nonblocking_ = o.send_nonblocking_;
    o.fd_ = -1;
  }
  return *this;
}

Channel::~Channel() {
  close();
}

void Channel::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

// Room for two fds of ancillary data.
static constexpr int kMaxFds = 2;

bool Channel::send(const Message& m,
                   std::span<const std::byte> extra,
                   int fd0,
                   int fd1) {
  if (fd_ < 0)
    return false;
  assert(extra.size() == m.extra_len && "extra.size() must equal m.extra_len");

  // The header and the trailer go in one datagram via two iovecs.
  iovec iov[2];
  iov[0].iov_base = const_cast<Message*>(&m);
  iov[0].iov_len = sizeof(Message);
  int niov = 1;
  if (!extra.empty()) {
    iov[1].iov_base = const_cast<std::byte*>(extra.data());
    iov[1].iov_len = extra.size();
    niov = 2;
  }

  int fds[kMaxFds];
  int nfds = 0;
  if (fd0 >= 0)
    fds[nfds++] = fd0;
  if (fd1 >= 0)
    fds[nfds++] = fd1;

  alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
  msghdr mh{};
  mh.msg_iov = iov;
  mh.msg_iovlen = niov;
  if (nfds > 0) {
    std::memset(cbuf, 0, sizeof(cbuf));
    mh.msg_control = cbuf;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
    cmsghdr* cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
    std::memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
    mh.msg_controllen = cm->cmsg_len;
  }

  ssize_t n;
  do {
    n = ::sendmsg(fd_, &mh,
                  MSG_NOSIGNAL | (send_nonblocking_ ? MSG_DONTWAIT : 0));
  } while (n < 0 && errno == EINTR);
  return n >= 0;
}

bool Channel::recv(Received& out, bool nonblock) {
  if (fd_ < 0)
    return false;
  out.extra.clear();
  out.fd0 = -1;
  out.fd1 = -1;

  // SEQPACKET preserves message boundaries; size the buffer to header + a
  // generous trailer.
  static constexpr size_t kMaxExtra = 64 * 1024;
  // One buffer per thread, not one 64 KiB allocation per datagram.
  thread_local std::vector<std::byte> buf(sizeof(Message) + kMaxExtra);

  iovec iov;
  iov.iov_base = buf.data();
  iov.iov_len = buf.size();

  alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
  msghdr mh{};
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  mh.msg_control = cbuf;
  mh.msg_controllen = sizeof(cbuf);

  ssize_t n;
  do {
    n = ::recvmsg(fd_, &mh, MSG_CMSG_CLOEXEC | (nonblock ? MSG_DONTWAIT : 0));
  } while (n < 0 && errno == EINTR);
  if (n < 0)
    return false;
  // Harvest any passed fds (up to kMaxFds) before anything can return: the
  // kernel has already installed them in this process, whatever the datagram
  // turns out to hold.
  int recvd[kMaxFds];
  int nfds = 0;
  for (cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm != nullptr;
       cm = CMSG_NXTHDR(&mh, cm)) {
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS &&
        cm->cmsg_len >= CMSG_LEN(0)) {
      const size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      for (size_t i = 0; i < count; ++i) {
        int f;
        std::memcpy(&f, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
        if (nfds < kMaxFds)
          recvd[nfds++] = f;
        else
          ::close(f);  // more than a message carries; not ours to keep
      }
    }
  }

  // Zero bytes is the peer closing. An empty datagram reads the same way, and
  // a peer that sends one has ended the conversation just as well.
  if (static_cast<size_t>(n) < sizeof(Message)) {  // EOF, truncated or bogus
    for (int i = 0; i < nfds; ++i)
      ::close(recvd[i]);
    errno = n == 0 ? 0 : EPROTO;
    return false;
  }

  std::memcpy(&out.msg, buf.data(), sizeof(Message));
  size_t extra_have = static_cast<size_t>(n) - sizeof(Message);
  // Trust the smaller of what arrived vs what the header claims.
  size_t extra_len =
      out.msg.extra_len < extra_have ? out.msg.extra_len : extra_have;
  if (extra_len > 0)
    out.extra.assign(buf.begin() + sizeof(Message),
                     buf.begin() + sizeof(Message) + extra_len);

  if (nfds > 0)
    out.fd0 = recvd[0];
  if (nfds > 1)
    out.fd1 = recvd[1];
  return true;
}

std::pair<Channel, Channel> Channel::make_pair() {
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
    return {Channel{}, Channel{}};
  }
  return {Channel{sv[0]}, Channel{sv[1]}};
}

}  // namespace acl::ipc

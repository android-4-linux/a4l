// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md). Tier-A unit test
// for the acl_ipc transport — the art-host <-> acl seam. No GPL-3.0 sources.
//
// The crux of the split-process design is that a buffer (dma-buf) fd can cross
// the process boundary via SCM_RIGHTS. These tests prove: (1) header + trailing
// bytes round-trip over SEQPACKET; (2) a passed fd on the receiving end refers
// to the SAME kernel object as the sender's (write through the received fd,
// read back through the sender's original — as a real cross-process dma-buf
// would).

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>

#include "ipc/ipc.h"

using namespace acl::ipc;

TEST_CASE("acl_ipc round-trips a header-only message", "[ipc]") {
  auto [a, b] = Channel::make_pair();
  REQUIRE(a.valid());
  REQUIRE(b.valid());

  Message m;
  m.type = MsgType::Input;
  m.input = Input{/*kind=*/0,  /*action=*/1,  /*x=*/12.5f,
                  /*y=*/34.0f, /*keycode=*/0, /*ts=*/99};
  REQUIRE(a.send(m));

  Received r;
  REQUIRE(b.recv(r));
  REQUIRE(r.msg.type == MsgType::Input);
  REQUIRE(r.msg.input.x == 12.5f);
  REQUIRE(r.msg.input.y == 34.0f);
  REQUIRE(r.msg.input.action == 1);
  REQUIRE(r.fd0 == -1);
  REQUIRE(r.extra.empty());
}

TEST_CASE("acl_ipc carries trailing bytes (Log payload)", "[ipc]") {
  auto [a, b] = Channel::make_pair();
  const char* text = "acl-split: hello from art-host";
  auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text), std::strlen(text));

  Message m;
  m.type = MsgType::Log;
  m.extra_len = static_cast<std::uint32_t>(bytes.size());
  REQUIRE(a.send(m, bytes));

  Received r;
  REQUIRE(b.recv(r));
  REQUIRE(r.msg.type == MsgType::Log);
  REQUIRE(r.extra.size() == bytes.size());
  std::string got(reinterpret_cast<const char*>(r.extra.data()),
                  r.extra.size());
  REQUIRE(got == text);
}

TEST_CASE("acl_ipc passes an fd that shares the sender's kernel object",
          "[ipc]") {
  auto [a, b] = Channel::make_pair();

  // A pipe stands in for a dma-buf: the write end is "presented" across the
  // boundary; if the received fd is a genuine dup of the same object, bytes
  // written through it appear on our read end.
  int p[2];
  REQUIRE(::pipe(p) == 0);
  int read_end = p[0];
  int write_end = p[1];

  Message m;
  m.type = MsgType::PresentFrame;
  m.present = PresentFrame{/*surface_id=*/7,
                           /*buffer_id=*/3,
                           /*w=*/64,
                           /*h=*/48,
                           /*drm_format=*/0x34325258 /*XR24*/,
                           /*modifier=*/0,
                           /*stride=*/256,
                           /*offset=*/0,
                           0,
                           0,
                           64,
                           48};
  REQUIRE(a.send(m, {}, /*fd0=*/write_end));
  ::close(write_end);  // sender drops its copy; the receiver now holds the only
                       // writer

  Received r;
  REQUIRE(b.recv(r));
  REQUIRE(r.msg.type == MsgType::PresentFrame);
  REQUIRE(r.msg.present.surface_id == 7);
  REQUIRE(r.msg.present.buffer_id == 3);
  REQUIRE(r.msg.present.w == 64);
  REQUIRE(r.msg.present.h == 48);
  REQUIRE(r.fd0 >= 0);  // an fd came across

  // Prove it's the SAME object: write through the received fd, read from our
  // original read end.
  const char probe[4] = {'A', 'C', 'L', '!'};
  REQUIRE(::write(r.fd0, probe, sizeof(probe)) ==
          static_cast<ssize_t>(sizeof(probe)));
  char got[4] = {};
  REQUIRE(::read(read_end, got, sizeof(got)) ==
          static_cast<ssize_t>(sizeof(got)));
  REQUIRE(std::memcmp(got, probe, sizeof(probe)) == 0);

  ::close(r.fd0);
  ::close(read_end);
}

TEST_CASE("acl_ipc recv reports clean EOF when the peer closes", "[ipc]") {
  auto [a, b] = Channel::make_pair();
  a.close();  // peer goes away
  Received r;
  REQUIRE_FALSE(b.recv(r));  // clean EOF -> false (errno 0)
}

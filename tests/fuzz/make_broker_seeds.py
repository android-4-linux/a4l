#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l tooling (docs/adr/ADR-0009 C5). No GPL-3.0 sources.
"""Write seed inputs for tests/fuzz/broker_fuzz.cpp.

  make_broker_seeds.py <directory>

Each seed is a script of datagrams in the harness's record format (one byte
of descriptor selection, a little-endian u16 length, the datagram). The seeds
are well-formed sessions, so that the fuzzer starts from the paths that work
and mutates outward. The layout below is acl::ipc::Message on x86-64 and
arm64; if src/ipc/ipc.h changes, change it here.
"""
import os
import struct
import sys

MESSAGE_SIZE = 96
HELLO, BYE, LOG, SURFACE_CREATE, SURFACE_RESIZE, PRESENT_FRAME = 1, 2, 3, 4, 5, 6
KEEPALIVE, IME_VISIBLE, CLIPBOARD = 15, 16, 17
VIDEO_FRAME, VIDEO_BOUNDS, VIDEO_HIDE, VIDEO_RESET = 18, 19, 20, 21
AR24 = 0x34325241
NV12 = 0x3231564E
TILED = 0x0200000000401B03
BIG, SMALL, NULL = 0, 1, 2  # descriptor kinds, in the harness's order


def message(tag, payload=b"", extra=b""):
    head = struct.pack("<II", tag, len(extra)) + payload
    return head.ljust(MESSAGE_SIZE, b"\0") + extra


def record(datagram, fds=0, kind=BIG):
    return bytes([(kind << 2) | fds]) + struct.pack("<H", len(datagram)) + datagram


def surface(w, h, title=b"Clock", app_id=b"org.example.clock"):
    return message(SURFACE_CREATE, struct.pack("<IIII", 1, w, h, 1),
                   title + b"\0" + app_id)


def frame(w, h, slot=0, stride=None, offset=0, modifier=TILED, fmt=AR24,
          planes=1):
    stride = w * 4 if stride is None else stride
    body = struct.pack("<IIIII4xQII4iI3I3I", 1, slot & 0xFFFFFFFF, w, h, fmt,
                       modifier, stride, offset, 0, 0, w, h, planes,
                       0, 0, 0, 0, 0, 0)
    return message(PRESENT_FRAME, body)


def video(w, h, fmt=NV12, modifier=0, planes=2, dest=None, stride=None,
          chroma_offset=None):
    stride = w if stride is None else stride
    chroma_offset = stride * h if chroma_offset is None else chroma_offset
    dest = (0, 0, w, h) if dest is None else dest
    body = struct.pack("<IIIIQ4I4I4i", w, h, fmt, planes, modifier,
                       stride, stride, 0, 0, 0, chroma_offset, 0, 0, *dest)
    return message(VIDEO_FRAME, body)


def bounds(x, y, w, h):
    return message(VIDEO_BOUNDS, struct.pack("<4i", x, y, w, h))


def seeds():
    hello = record(message(HELLO, struct.pack("<I", 2)))
    yield "session", (hello + record(surface(640, 480)) +
                      b"".join(record(frame(640, 480, s), 1) for s in (0, 1, 2))
                      + record(message(KEEPALIVE)) + record(message(BYE)))
    yield "linear", (record(surface(320, 200)) +
                     record(frame(320, 200, -1, modifier=0), 1))
    yield "two-planes", (record(surface(640, 480)) +
                         record(frame(640, 480, 0, planes=2)[:68] +
                                struct.pack("<3I3I", 1280, 0, 0, 4096, 0, 0) +
                                b"\0" * 4, 1))
    yield "fence", record(surface(640, 480)) + record(frame(640, 480), 2)
    yield "clipboard", (record(surface(64, 64)) +
                        record(message(CLIPBOARD, extra=b"copied text")) +
                        record(message(IME_VISIBLE, struct.pack("<I", 1))))
    yield "resurface", b"".join(record(surface(64 + i, 64)) for i in range(4))
    # The refusals, one each, so the fuzzer has them without finding them.
    yield "no-surface", record(frame(640, 480), 1)
    yield "wrong-size", record(surface(640, 480)) + record(frame(1280, 960), 1)
    yield "small-buffer", (record(surface(640, 480)) +
                           record(frame(640, 480), 1, SMALL))
    yield "not-a-buffer", (record(surface(640, 480)) +
                           record(frame(640, 480), 1, NULL))
    yield "short", record(surface(640, 480)) + record(b"\x06\0\0", 2)
    # The video plane: a clip played, moved, hidden and reset.
    yield "video", (record(surface(1280, 800)) + record(bounds(120, 164, 640, 360)) +
                    b"".join(record(video(640, 360), 1) for _ in range(3)) +
                    record(bounds(-640, 0, 2560, 1440)) +
                    record(video(640, 360, modifier=TILED), 1) +
                    record(message(VIDEO_HIDE)) + record(message(VIDEO_RESET)))
    yield "video-and-ui", (record(surface(640, 480)) +
                           record(frame(640, 480, 0), 1) +
                           record(video(1920, 1080, dest=(0, 0, 640, 360)), 1) +
                           record(frame(640, 480, 1), 1))
    yield "video-refused", (record(video(640, 360), 1) +
                            record(surface(640, 480)) +
                            record(video(640, 360, fmt=AR24), 1) +
                            record(video(640, 360, modifier=0x33), 1) +
                            record(video(640, 360, planes=1), 1) +
                            record(video(640, 360), 1, SMALL) +
                            record(video(640, 360), 1, NULL) +
                            record(video(640, 360, dest=(0, 0, 0, 0)), 1) +
                            record(bounds(0, 0, -1, 0x7FFFFFFF)))
    yield "titles", record(surface(64, 64, b"\xe2\x80\xaeevil\x1b[2J\xff",
                                   b"\xc0\xaf\x80"))


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    os.makedirs(sys.argv[1], exist_ok=True)
    for name, data in seeds():
        with open(os.path.join(sys.argv[1], name), "wb") as f:
            f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())

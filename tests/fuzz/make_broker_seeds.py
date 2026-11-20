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
AUDIO_OPEN, AUDIO_WRITE, AUDIO_CLOSE = 11, 12, 22
AR24 = 0x34325241
NV12 = 0x3231564E
XB24 = 0x34324258
TILED = 0x0200000000401B03
SAND128 = 0x0700000000000004  # Broadcom's column format, no height in the modifier
BIG, SMALL, NULL = 0, 1, 2  # descriptor kinds, in the harness's order


def message(tag, payload=b"", extra=b""):
    head = struct.pack("<II", tag, len(extra)) + payload
    return head.ljust(MESSAGE_SIZE, b"\0") + extra


def record(datagram, fds=0, kind=BIG, attached=False):
    # attached: on an input's first record, the child stays connected (broker_fuzz.cpp).
    sel = (kind << 2) | fds | (0x80 if attached else 0)
    return bytes([sel]) + struct.pack("<H", len(datagram)) + datagram


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
          chroma_offset=None, chroma_buffer=0):
    # chroma_buffer 1: the chroma is in the second descriptor sent, at its start.
    stride = w if stride is None else stride
    if chroma_offset is None:
        chroma_offset = 0 if chroma_buffer else stride * h
    dest = (0, 0, w, h) if dest is None else dest
    # The last field is the frame's id: nonzero, so that the broker answers with
    # VideoReleased when it is done with the frame.
    # Then the stream's matrix (1: BT.709) and range (0: narrow).
    body = struct.pack("<IIIIQ4I4I4i4IIII", w, h, fmt, planes, modifier,
                       stride, stride, 0, 0, 0, chroma_offset, 0, 0, *dest,
                       0, chroma_buffer, 0, 0, 7, 1, 0)
    return message(VIDEO_FRAME, body)


def sand_two(w, h, luma_rows, chroma_rows):
    # A column-format frame with a buffer per plane, each plane at its buffer's start.
    body = struct.pack("<IIIIQ4I4I4i4I", w, h, NV12, 2, SAND128,
                       luma_rows, chroma_rows, 0, 0, 0, 0, 0, 0, 0, 0, w, h,
                       0, 1, 0, 0)
    return message(VIDEO_FRAME, body)


def converted(w, h, modifier=0, planes=1, stride=None, offset=0, aux=(0, 0),
              dest=None):
    # A picture the child converted to XBGR8888: one plane of 32-bit pixels. aux is the
    # stride and offset of a second, auxiliary plane.
    stride = w * 4 if stride is None else stride
    dest = (0, 0, w, h) if dest is None else dest
    body = struct.pack("<IIIIQ4I4I4i", w, h, XB24, planes, modifier,
                       stride, aux[0], 0, 0, offset, aux[1], 0, 0, *dest)
    return message(VIDEO_FRAME, body)


def bounds(x, y, w, h):
    return message(VIDEO_BOUNDS, struct.pack("<4i", x, y, w, h))


def audio_open(stream, rate=44100, channels=2):
    return message(AUDIO_OPEN, struct.pack("<III", stream, rate, channels))


def audio_write(stream, frames=64, channels=2):
    pcm = struct.pack("<%dh" % (frames * channels),
                      *[(i * 257) % 30000 - 15000 for i in range(frames * channels)])
    return message(AUDIO_WRITE, struct.pack("<I", stream), pcm)


def audio_close(stream):
    return message(AUDIO_CLOSE, struct.pack("<I", stream))


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
    # A decoder with luma and chroma in a buffer each: two descriptors, the big one and
    # the 4096-byte one. A 64x64 picture's chroma fits the second; the others do not, or
    # do not say which plane is where.
    yield "video-two-buffers", (
        record(surface(1280, 800)) +
        record(video(64, 64, chroma_buffer=1), 2) +
        record(video(128, 128, chroma_buffer=1), 2) +
        record(video(64, 64, chroma_buffer=1, chroma_offset=2049), 2) +
        record(video(64, 64, chroma_buffer=1), 1) +
        record(video(64, 64), 2) +
        record(video(64, 64, chroma_buffer=2), 2) +
        record(video(64, 64, chroma_buffer=1), 3) +
        record(video(64, 64, chroma_buffer=1), 2, SMALL) +
        record(message(VIDEO_HIDE)))
    # Broadcom's column format, as a Pi 5's HEVC decoder gives it: the stride is a column's
    # height in rows. 640x360 in one buffer (552-row columns, chroma 368 rows down), a
    # buffer per plane with the chroma in the 4096-byte descriptor (64x64: one column of 32
    # rows), then frames whose columns do not fit.
    yield "video-sand", (
        record(surface(1280, 800)) +
        record(video(640, 360, modifier=SAND128, stride=552, chroma_offset=368 * 128), 1) +
        record(sand_two(64, 64, 64, 32), 2) +
        record(video(640, 360, modifier=SAND128, stride=359, chroma_offset=368 * 128), 1) +
        record(video(640, 360, modifier=SAND128, stride=0xFFFFFFFF, chroma_offset=0), 1) +
        record(video(640, 360, modifier=SAND128, stride=552, chroma_offset=0xFFFFFF00), 1) +
        record(video(16384, 360, modifier=SAND128, stride=552, chroma_offset=368 * 128), 1) +
        record(sand_two(128, 128, 128, 64), 2) +
        record(video(640, 360, modifier=SAND128 | (552 << 8), stride=552,
                     chroma_offset=368 * 128), 1) +
        record(message(VIDEO_HIDE)))
    # Converted video: the compositor takes XBGR8888 LINEAR on the video plane. A clip
    # played, with an auxiliary plane, after a decoder frame, then every way to get the
    # one-plane layout wrong.
    yield "video-converted", (record(surface(1280, 800)) +
                              record(bounds(120, 164, 640, 360)) +
                              b"".join(record(converted(640, 360), 1) for _ in range(3)) +
                              record(converted(640, 360, planes=2, aux=(64, 4096)), 1) +
                              record(video(640, 360), 1) +
                              record(converted(640, 360, stride=2592, offset=4096), 1) +
                              record(message(VIDEO_HIDE)))
    yield "video-converted-refused", (
        record(surface(640, 480)) +
        record(converted(640, 360, modifier=TILED), 1) +
        record(converted(640, 360, stride=640), 1) +
        record(converted(640, 360, offset=4 << 20), 1) +
        record(converted(640, 360, planes=0), 1) +
        record(converted(640, 360, planes=5), 1) +
        record(converted(640, 360, planes=2, aux=(0, 0)), 1) +
        record(converted(640, 360, planes=2, aux=(64, 4 << 20)), 1) +
        record(converted(640, 360), 1, SMALL) +
        record(converted(8192, 8192), 1) +
        record(converted(640, 360, dest=(0, 0, 0, 0)), 1) +
        record(converted(640, 360), 1))
    # Audio: two streams played and closed, with a window or without.
    # The harness's compositor asks for half the size after the first frame: the new size,
    # the size left behind (refused from then on), the new size again.
    yield "resize", (record(surface(64, 64), attached=True) + record(frame(64, 64), 1) +
                     record(frame(32, 32, 1), 1) + record(frame(64, 64, 2), 1) +
                     record(frame(32, 32, 0), 1))
    yield "audio", (record(audio_open(0)) + record(audio_write(0)) +
                    record(audio_open(1, 48000, 1)) +
                    record(audio_write(1, 100, 1)) + record(audio_write(0, 512)) +
                    record(audio_close(0)) + record(audio_close(1)))
    yield "audio-and-ui", (record(surface(640, 480)) + record(audio_open(3, 8000, 6)) +
                           record(frame(640, 480, 0), 1) +
                           record(audio_write(3, 32, 6)) + record(message(BYE)))
    yield "audio-refused", (record(audio_write(0)) + record(audio_open(8)) +
                            record(audio_open(0, 0, 2)) + record(audio_open(0, 44100, 0)) +
                            record(audio_open(0)) +
                            record(message(AUDIO_WRITE, struct.pack("<I", 0), b"\x01\x02\x03")) +
                            record(audio_write(0, 3, 1)) + record(audio_close(99)) +
                            record(audio_open(0, 96000, 1)) + record(audio_write(0, 9, 1)))
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

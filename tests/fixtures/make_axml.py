# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; AOSP androidfw ResourceTypes.h
# binary-XML chunk layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Encode a small, known AndroidManifest.xml in Android's compiled binary-XML
# (AXML) format, for the pkg/manifest parser tests. Independent of the C++
# decoder (both follow the documented format), so it is a real cross-check.
#
# Usage: make_axml.py [--utf16] <output.axml>
import struct
import sys

# ResChunk_header.type
RES_STRING_POOL = 0x0001
RES_XML = 0x0003
RES_XML_RESOURCE_MAP = 0x0180
RES_XML_START_NS = 0x0100
RES_XML_END_NS = 0x0101
RES_XML_START_ELEM = 0x0102
RES_XML_END_ELEM = 0x0103

UTF8_FLAG = 1 << 8
TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
NO_ENTRY = 0xFFFFFFFF
ANDROID_URI = "http://schemas.android.com/apk/res/android"

# Framework attribute names placed first, paired with their resource ids so a
# resource-map chunk can be emitted (as real AXML does).
ATTR_NAMES = [
    ("versionCode", 0x0101021B),
    ("versionName", 0x0101021C),
    ("compileSdkVersion", 0x01010572),
    ("minSdkVersion", 0x0101020C),
    ("targetSdkVersion", 0x01010270),
    ("name", 0x01010003),
    ("label", 0x01010001),
]


class Pool:
    def __init__(self):
        self.strings = []
        self.index = {}
        for name, _ in ATTR_NAMES:
            self._add(name)

    def _add(self, s):
        self.index[s] = len(self.strings)
        self.strings.append(s)
        return self.index[s]

    def sid(self, s):
        if s not in self.index:
            self._add(s)
        return self.index[s]


def _len8(n):
    if n < 0x80:
        return bytes([n])
    return bytes([0x80 | (n >> 8), n & 0xFF])


def _len16(n):
    if n < 0x8000:
        return struct.pack("<H", n)
    return struct.pack("<HH", 0x8000 | (n >> 16), n & 0xFFFF)


def string_pool(pool, utf8):
    data = b""
    offsets = []
    for s in pool.strings:
        offsets.append(len(data))
        if utf8:
            b = s.encode("utf-8")
            data += _len8(len(s)) + _len8(len(b)) + b + b"\x00"
        else:
            u = s.encode("utf-16-le")
            data += _len16(len(u) // 2) + u + b"\x00\x00"
    while len(data) % 4 != 0:
        data += b"\x00"
    header_size = 28
    strings_start = header_size + 4 * len(pool.strings)
    size = strings_start + len(data)
    out = struct.pack("<HHI", RES_STRING_POOL, header_size, size)
    flags = UTF8_FLAG if utf8 else 0
    out += struct.pack("<IIIII", len(pool.strings), 0, flags, strings_start, 0)
    for off in offsets:
        out += struct.pack("<I", off)
    return out + data


def resource_map():
    ids = [rid for _, rid in ATTR_NAMES]
    out = struct.pack("<HHI", RES_XML_RESOURCE_MAP, 8, 8 + 4 * len(ids))
    for rid in ids:
        out += struct.pack("<I", rid)
    return out


def _node(node_type, ext):
    header_size = 16
    size = header_size + len(ext)
    return struct.pack("<HHIII", node_type, header_size, size, 0, NO_ENTRY) + ext


def _attribute(pool, ns_idx, name_idx, value):
    kind, v = value
    if kind == "str":
        raw = pool.sid(v)
        vtype, data = TYPE_STRING, pool.sid(v)
    else:
        raw, vtype, data = NO_ENTRY, TYPE_INT_DEC, v & 0xFFFFFFFF
    return struct.pack("<IIIHBBI", ns_idx, name_idx, raw, 8, 0, vtype, data)


def start_elem(pool, name, attrs):
    ext = struct.pack("<IIHHHHHH", NO_ENTRY, pool.sid(name), 20, 20, len(attrs),
                      0, 0, 0)
    for a in attrs:
        ext += a
    return _node(RES_XML_START_ELEM, ext)


def end_elem(pool, name):
    return _node(RES_XML_END_ELEM, struct.pack("<II", NO_ENTRY, pool.sid(name)))


def build_axml(utf8=True, package="com.example.app", launcher=".MainActivity"):
    pool = Pool()
    uri = lambda: pool.sid(ANDROID_URI)          # noqa: E731
    a = lambda name, val: _attribute(pool, uri(), pool.sid(name), val)  # noqa: E731
    pkg = lambda: _attribute(pool, NO_ENTRY, pool.sid("package"),  # noqa: E731
                             ("str", package))

    body = b""
    body += _node(RES_XML_START_NS,
                  struct.pack("<II", pool.sid("android"), uri()))
    body += start_elem(pool, "manifest", [
        pkg(),
        a("versionCode", ("int", 42)),
        a("versionName", ("str", "1.2.3")),
        a("compileSdkVersion", ("int", 33)),
    ])
    body += start_elem(pool, "uses-sdk", [
        a("minSdkVersion", ("int", 24)),
        a("targetSdkVersion", ("int", 33)),
    ])
    body += end_elem(pool, "uses-sdk")
    body += start_elem(pool, "application", [a("label", ("str", "Example"))])
    body += start_elem(pool, "activity", [a("name", ("str", launcher))])
    body += start_elem(pool, "intent-filter", [])
    body += start_elem(pool, "action",
                       [a("name", ("str", "android.intent.action.MAIN"))])
    body += end_elem(pool, "action")
    body += start_elem(pool, "category",
                       [a("name", ("str", "android.intent.category.LAUNCHER"))])
    body += end_elem(pool, "category")
    body += end_elem(pool, "intent-filter")
    body += end_elem(pool, "activity")
    body += start_elem(pool, "activity",
                       [a("name", ("str", "com.example.app.OtherActivity"))])
    body += end_elem(pool, "activity")
    body += end_elem(pool, "application")
    body += end_elem(pool, "manifest")
    body += _node(RES_XML_END_NS,
                  struct.pack("<II", pool.sid("android"), uri()))

    # The string pool must be serialized after the body so every referenced
    # string has been interned; it precedes the body in the file.
    pool_bytes = string_pool(pool, utf8) + resource_map()
    payload = pool_bytes + body
    return struct.pack("<HHI", RES_XML, 8, 8 + len(payload)) + payload


def main():
    args = sys.argv[1:]
    utf8 = True
    if args and args[0] == "--utf16":
        utf8 = False
        args = args[1:]
    if len(args) != 1:
        sys.stderr.write("usage: make_axml.py [--utf16] <output.axml>\n")
        return 2
    with open(args[0], "wb") as f:
        f.write(build_axml(utf8=utf8))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

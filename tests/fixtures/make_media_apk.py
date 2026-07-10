# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK with a media entry point (Gate G2.e, automotive media path):
#   package com.aclcxx.hello;
#   public class MusicService extends android.service.media.MediaBrowserService {
#     public BrowserRoot onGetRoot(String pkg, int uid, android.os.Bundle hints) {
#       return new BrowserRoot("aclcxx_root", null);
#     }
#   }
# `aclcxx run media.apk --service=com.aclcxx.hello.MusicService` hosts it in-process
# (no Views, no Binder): onCreate() then onGetRoot() -> the browse root. Independent
# of the C++ decoder.
#
# Usage: make_media_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

BROWSER_ROOT = "Landroid/service/media/MediaBrowserService$BrowserRoot;"
STRINGS = [
    "Lcom/aclcxx/hello/MusicService;",              # 0
    "Landroid/service/media/MediaBrowserService;",  # 1  superclass
    BROWSER_ROOT,                                   # 2  nested return type
    "Landroid/os/Bundle;",                          # 3
    "Ljava/lang/String;",                           # 4
    "V",                                            # 5
    "I",                                            # 6
    "<init>",                                       # 7
    "onGetRoot",                                    # 8
    "aclcxx_root",                                  # 9  browse root id
    "LLIL",                                         # 10 shorty (String,int,Bundle)->BrowserRoot
    "VLL",                                          # 11 shorty (String,Bundle)->void
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = ["Lcom/aclcxx/hello/MusicService;",
         "Landroid/service/media/MediaBrowserService;", BROWSER_ROOT,
         "Landroid/os/Bundle;", "Ljava/lang/String;", "V", "I"]
T = {t: i for i, t in enumerate(TYPES)}


def uleb128(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def align4(buf):
    while len(buf) % 4 != 0:
        buf += b"\x00"
    return buf


def type_list(types):
    return align4(bytearray(struct.pack("<I", len(types))
                            + b"".join(struct.pack("<H", T[t]) for t in types)))


def code_item(regs, ins, outs, insns):
    body = struct.pack("<HHHHII", regs, ins, outs, 0, 0, len(insns))
    for u in insns:
        body += struct.pack("<H", u)
    return align4(bytearray(body))


def build_dex():
    header_size = 112
    n_strings, n_types, n_protos, n_fields, n_methods, n_classdefs = \
        len(STRINGS), len(TYPES), 3, 0, 4, 1

    off_string_ids = header_size
    off_type_ids = off_string_ids + n_strings * 4
    off_proto_ids = off_type_ids + n_types * 4
    off_field_ids = off_proto_ids + n_protos * 12
    off_method_ids = off_field_ids + n_fields * 8
    off_class_defs = off_method_ids + n_methods * 8
    off_data = off_class_defs + n_classdefs * 32

    data = bytearray()
    string_data_off = []
    for s in STRINGS:
        b = s.encode("utf-8")
        string_data_off.append(off_data + len(data))
        data.extend(uleb128(len(s)) + b + b"\x00")

    data.extend(align4(bytearray()))
    tl_getroot_off = off_data + len(data)         # (String, int, Bundle)
    data.extend(type_list(["Ljava/lang/String;", "I", "Landroid/os/Bundle;"]))
    data.extend(align4(bytearray()))
    tl_rootctor_off = off_data + len(data)        # (String, Bundle)
    data.extend(type_list(["Ljava/lang/String;", "Landroid/os/Bundle;"]))

    # <init>()V : super(); return-void      (v0 = this)
    code_init_off = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0002, 0x0000,   # invoke-direct {v0}, MediaBrowserService.<init>() (m2)
        0x000E,                   # return-void
    ]))
    # BrowserRoot onGetRoot(String,int,Bundle) : return new BrowserRoot("aclcxx_root", null)
    #   this=v3, pkg=v4, uid=v5, hints=v6 ; temps v0,v1,v2
    code_getroot_off = off_data + len(data)
    data.extend(code_item(7, 4, 3, [
        0x0022, 0x0002,           # new-instance v0, BrowserRoot (type 2)
        0x011A, S["aclcxx_root"], # const-string v1, "aclcxx_root"
        0x0212,                   # const/4 v2, 0  (null Bundle)
        0x3070, 0x0003, 0x0210,   # invoke-direct {v0,v1,v2}, BrowserRoot.<init>(String,Bundle) (m3)
        0x0011,                   # return-object v0
    ]))

    class_data_off = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(0) + uleb128(0x1) + uleb128(code_init_off)     # m0 <init>, public
    cd += uleb128(1) + uleb128(0x1) + uleb128(code_getroot_off)  # +1 -> m1 onGetRoot, public
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0),
        (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids),
        (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids),
        (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]),
        (0x1001, 2, tl_getroot_off),
        (0x2001, 2, code_init_off),
        (0x2000, 1, class_data_off),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)
    data.extend(align4(map_body))
    data_size = len(data)

    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)
    proto_ids = struct.pack("<III", S["V"], T["V"], 0)                      # 0 ()V
    proto_ids += struct.pack("<III", S["LLIL"], T[BROWSER_ROOT], tl_getroot_off)  # 1 onGetRoot
    proto_ids += struct.pack("<III", S["VLL"], T["V"], tl_rootctor_off)     # 2 BrowserRoot.<init>
    method_ids = struct.pack("<HHI", T["Lcom/aclcxx/hello/MusicService;"], 0, S["<init>"])     # m0
    method_ids += struct.pack("<HHI", T["Lcom/aclcxx/hello/MusicService;"], 1, S["onGetRoot"])  # m1
    method_ids += struct.pack("<HHI", T["Landroid/service/media/MediaBrowserService;"], 0, S["<init>"])  # m2
    method_ids += struct.pack("<HHI", T[BROWSER_ROOT], 2, S["<init>"])       # m3 BrowserRoot.<init>
    class_defs = struct.pack("<IIIIIIII",
                             T["Lcom/aclcxx/hello/MusicService;"], 0x1,
                             T["Landroid/service/media/MediaBrowserService;"], 0,
                             0xFFFFFFFF, 0, class_data_off, 0)

    header = bytearray(header_size)
    header[0:8] = b"dex\n035\x00"
    struct.pack_into("<I", header, 0x24, header_size)
    struct.pack_into("<I", header, 0x28, 0x12345678)
    struct.pack_into("<I", header, 0x34, map_off)
    struct.pack_into("<I", header, 0x38, n_strings)
    struct.pack_into("<I", header, 0x3C, off_string_ids)
    struct.pack_into("<I", header, 0x40, n_types)
    struct.pack_into("<I", header, 0x44, off_type_ids)
    struct.pack_into("<I", header, 0x48, n_protos)
    struct.pack_into("<I", header, 0x4C, off_proto_ids)
    struct.pack_into("<I", header, 0x50, n_fields)
    struct.pack_into("<I", header, 0x54, 0)
    struct.pack_into("<I", header, 0x58, n_methods)
    struct.pack_into("<I", header, 0x5C, off_method_ids)
    struct.pack_into("<I", header, 0x60, n_classdefs)
    struct.pack_into("<I", header, 0x64, off_class_defs)
    struct.pack_into("<I", header, 0x68, data_size)
    struct.pack_into("<I", header, 0x6C, off_data)

    dex = bytearray(header) + string_ids + type_ids + proto_ids + \
        method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))
    dex[12:32] = hashlib.sha1(bytes(dex[32:])).digest()
    struct.pack_into("<I", dex, 0x08, zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF)
    return bytes(dex)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_media_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.aclcxx.hello",
            launcher="com.aclcxx.hello.MusicService"),
        "classes.dex": build_dex(),
    }
    with zipfile.ZipFile(sys.argv[1], "w") as z:
        for name, blob in entries.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, blob)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

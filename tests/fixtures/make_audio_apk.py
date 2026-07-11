# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK that plays audio (Gate G2.e, media audio):
#   package com.acl.hello;
#   public class AudioApp {
#     public static void main(String[] args) {
#       short[] pcm = new short[44100];              // 1s mono @ 44100
#       for (int i = 0; i < 44100; i++)
#         pcm[i] = (short)(((i % 100) * 600) - 30000);  // ~441 Hz sawtooth
#       AudioTrack t = new AudioTrack(44100, 1);
#       t.play(); t.write(pcm, 0, 44100); t.stop(); t.release();
#     }
#   }
# `acl run audio.apk --framework=<shims>` runs main() (AudioApp is not an
# Activity -> the launcher falls back to static main); AudioTrack's native side
# forwards the PCM to acl::audio (PipeWire when ACL_AUDIO=pipewire, else the
# counting sink). Independent of the C++ decoder.
#
# Usage: make_audio_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

STRINGS = [
    "Lcom/acl/hello/AudioApp;",  # 0
    "Ljava/lang/Object;",           # 1  super
    "Landroid/media/AudioTrack;",   # 2
    "[Ljava/lang/String;",          # 3  main param
    "[S",                           # 4  short[]
    "V",                            # 5
    "I",                            # 6
    "main",                         # 7
    "<init>",                       # 8
    "play",                         # 9
    "write",                        # 10
    "stop",                         # 11
    "release",                      # 12
    "VL",                           # 13 main shorty
    "VII",                          # 14 AudioTrack.<init>(int,int)
    "ILII",                         # 15 write ([S,int,int)->int
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = ["Lcom/acl/hello/AudioApp;", "Ljava/lang/Object;",
         "Landroid/media/AudioTrack;", "[Ljava/lang/String;", "[S", "V", "I"]
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


def code_item(regs, ins, outs, insns):
    body = struct.pack("<HHHHII", regs, ins, outs, 0, 0, len(insns))
    for u in insns:
        body += struct.pack("<H", u)
    return align4(bytearray(body))


def build_dex():
    header_size = 112
    n_strings = len(STRINGS)
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 4, 0, 6, 1

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

    def type_list(types):
        data.extend(align4(bytearray()))
        off = off_data + len(data)
        data.extend(align4(bytearray(
            struct.pack("<I", len(types)) + b"".join(struct.pack("<H", T[t]) for t in types))))
        return off

    tl_argv = type_list(["[Ljava/lang/String;"])
    tl_ii = type_list(["I", "I"])
    tl_write = type_list(["[S", "I", "I"])

    # main([String[])V : a tone loop into pcm, then play it through AudioTrack.
    #   v0=pcm v1=i v2=tmp/track v3=44100 v4=const ; v5=args (registers=6, ins=1)
    code_main = off_data + len(data)
    data.extend(code_item(6, 1, 4, [
        0x0314, 0xAC44, 0x0000,   # const v3, 44100
        0x3023, T["[S"],          # new-array v0, v3, [S
        0x0112,                   # const/4 v1, 0
        0x3135, 0x000E,           # if-ge v1, v3, +14  (-> pc20)
        0x02DC, 0x6401,           # rem-int/lit8 v2, v1, 100
        0x22D2, 0x0258,           # mul-int/lit16 v2, v2, 600
        0x22D0, 0x8AD0,           # add-int/lit16 v2, v2, -30000
        0x228F,                   # int-to-short v2, v2
        0x0251, 0x0100,           # aput-short v2, v0, v1
        0x01D8, 0x0101,           # add-int/lit8 v1, v1, 1
        0xF328,                   # goto -13 (-> pc6)
        0x0222, T["Landroid/media/AudioTrack;"],  # new-instance v2, AudioTrack
        0x1412,                   # const/4 v4, 1  (channels)
        0x3070, 0x0001, 0x0432,   # invoke-direct {v2,v3,v4}, AudioTrack.<init> (m1)
        0x106E, 0x0002, 0x0002,   # invoke-virtual {v2}, play (m2)
        0x0412,                   # const/4 v4, 0  (offset)
        0x406E, 0x0003, 0x3402,   # invoke-virtual {v2,v0,v4,v3}, write (m3)
        0x106E, 0x0004, 0x0002,   # invoke-virtual {v2}, stop (m4)
        0x106E, 0x0005, 0x0002,   # invoke-virtual {v2}, release (m5)
        0x000E,                   # return-void
    ]))

    # class_data: 1 direct (main), 0 virtual
    class_data_off = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(0))
    cd += uleb128(0) + uleb128(0x9) + uleb128(code_main)  # m0 main, public static
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids), (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 3, tl_argv),
        (0x2001, 1, code_main), (0x2000, 1, class_data_off),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)
    data.extend(align4(map_body))
    data_size = len(data)

    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)
    proto_ids = struct.pack("<III", S["VL"], T["V"], tl_argv)       # 0 main
    proto_ids += struct.pack("<III", S["VII"], T["V"], tl_ii)       # 1 AudioTrack.<init>
    proto_ids += struct.pack("<III", S["V"], T["V"], 0)             # 2 ()V play/stop/release
    proto_ids += struct.pack("<III", S["ILII"], T["I"], tl_write)   # 3 write

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m("Lcom/acl/hello/AudioApp;", 0, "main"),          # m0
        m("Landroid/media/AudioTrack;", 1, "<init>"),         # m1
        m("Landroid/media/AudioTrack;", 2, "play"),           # m2
        m("Landroid/media/AudioTrack;", 3, "write"),          # m3
        m("Landroid/media/AudioTrack;", 2, "stop"),           # m4
        m("Landroid/media/AudioTrack;", 2, "release"),        # m5
    ])
    class_defs = struct.pack("<IIIIIIII",
                             T["Lcom/acl/hello/AudioApp;"], 0x1,
                             T["Ljava/lang/Object;"], 0, 0xFFFFFFFF, 0,
                             class_data_off, 0)

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
        sys.stderr.write("usage: make_audio_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.acl.hello",
            launcher="com.acl.hello.AudioApp"),
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

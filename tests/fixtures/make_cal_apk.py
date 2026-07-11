# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK with a Car App Library entry point (Gate G1.5b, automotive CAL path):
#   package com.acl.hello;
#   public class CarApp extends androidx.car.app.CarAppService {
#     public Session onCreateSession() { return new CarSession(); }
#   }
#   class CarSession extends androidx.car.app.Session {
#     public Screen onCreateScreen() { return new CarScreen(); }
#   }
#   class CarScreen extends androidx.car.app.Screen {
#     public Template onGetTemplate() {
#       ItemList list = new ItemList.Builder()
#           .addItem(new Row.Builder().setTitle("Play Music").build())
#           .addItem(new Row.Builder().setTitle("Browse Library").build())
#           .build();
#       return new ListTemplate.Builder().setTitle("Media").setSingleList(list).build();
#     }
#   }
# `acl run cal.apk --cal=com.acl.hello.CarApp` hosts it in-process (no Views, no
# Binder): onCreateSession() -> onCreateScreen() -> onGetTemplate(), and the native
# CAL host renders the template. Independent of the C++ decoder.
#
# Usage: make_cal_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

TYPES = [
    "Lcom/acl/hello/CarApp;",                        # 0
    "Lcom/acl/hello/CarSession;",                     # 1
    "Lcom/acl/hello/CarScreen;",                      # 2
    "Landroidx/car/app/CarAppService;",              # 3
    "Landroidx/car/app/Session;",                    # 4
    "Landroidx/car/app/Screen;",                     # 5
    "Landroidx/car/app/model/Template;",             # 6
    "Landroidx/car/app/model/ListTemplate;",         # 7
    "Landroidx/car/app/model/ListTemplate$Builder;",  # 8
    "Landroidx/car/app/model/ItemList;",             # 9
    "Landroidx/car/app/model/ItemList$Builder;",     # 10
    "Landroidx/car/app/model/Row;",                  # 11
    "Landroidx/car/app/model/Row$Builder;",          # 12
    "Ljava/lang/CharSequence;",                      # 13
    "V",                                             # 14
]
T = {t: i for i, t in enumerate(TYPES)}

STRINGS = TYPES + [
    "V",                 # shorty ()V           (dup of the "V" type descriptor)
    "L",                 # shorty ()L
    "LL",                # shorty (L)L
    "<init>",
    "onCreateSession",
    "onCreateScreen",
    "onGetTemplate",
    "setTitle",
    "addItem",
    "setSingleList",
    "build",
    "Play Music",
    "Browse Library",
    "Media",
]
S = {s: i for i, s in enumerate(STRINGS)}


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


N_PROTOS = 11
N_METHODS = 19
N_CLASSDEFS = 3


def build_dex():
    header_size = 112
    n_strings, n_types = len(STRINGS), len(TYPES)

    off_string_ids = header_size
    off_type_ids = off_string_ids + n_strings * 4
    off_proto_ids = off_type_ids + n_types * 4
    off_field_ids = off_proto_ids + N_PROTOS * 12
    off_method_ids = off_field_ids + 0 * 8
    off_class_defs = off_method_ids + N_METHODS * 8
    off_data = off_class_defs + N_CLASSDEFS * 32

    data = bytearray()
    string_data_off = []
    for s in STRINGS:
        b = s.encode("utf-8")
        string_data_off.append(off_data + len(data))
        data.extend(uleb128(len(s)) + b + b"\x00")

    def type_list(types):
        nonlocal data
        data.extend(align4(bytearray()))
        off = off_data + len(data)
        data.extend(align4(bytearray(
            struct.pack("<I", len(types)) + b"".join(struct.pack("<H", T[t]) for t in types))))
        return off

    tl_charseq = type_list(["Ljava/lang/CharSequence;"])
    tl_itemlist = type_list(["Landroidx/car/app/model/ItemList;"])
    tl_row = type_list(["Landroidx/car/app/model/Row;"])

    # <init>()V bodies (super call + return-void), one per subclass.
    def ctor(super_m):
        return code_item(1, 1, 1, [0x1070, super_m, 0x0000, 0x000E])

    code_app_init = off_data + len(data); data.extend(ctor(2))    # super CarAppService.<init> (m2)
    code_app_session = off_data + len(data)                        # new CarSession; return it
    data.extend(code_item(2, 1, 1, [
        0x0022, T["Lcom/acl/hello/CarSession;"],
        0x1070, 3, 0x0000,   # invoke-direct {v0}, CarSession.<init> (m3)
        0x0011,              # return-object v0
    ]))
    code_sess_init = off_data + len(data); data.extend(ctor(5))   # super Session.<init> (m5)
    code_sess_screen = off_data + len(data)                        # new CarScreen; return it
    data.extend(code_item(2, 1, 1, [
        0x0022, T["Lcom/acl/hello/CarScreen;"],
        0x1070, 6, 0x0000,   # invoke-direct {v0}, CarScreen.<init> (m6)
        0x0011,
    ]))
    code_screen_init = off_data + len(data); data.extend(ctor(8))  # super Screen.<init> (m8)

    # onGetTemplate(): build a ListTemplate with two rows. v4=this (regs=5, ins=1).
    def row(title):
        return [
            0x0122, T["Landroidx/car/app/model/Row$Builder;"],  # new-instance v1, Row$Builder
            0x1070, 12, 0x0001,   # invoke-direct {v1}, Row$Builder.<init> (m12)
            0x021A, S[title],     # const-string v2, title
            0x206E, 13, 0x0021,   # invoke-virtual {v1,v2}, Row$Builder.setTitle (m13)
            0x010C,               # move-result-object v1
            0x106E, 14, 0x0001,   # invoke-virtual {v1}, Row$Builder.build (m14)
            0x030C,               # move-result-object v3 (Row)
            0x206E, 10, 0x0030,   # invoke-virtual {v0,v3}, ItemList$Builder.addItem (m10)
        ]
    code_screen_tmpl = off_data + len(data)
    insns = [
        0x0022, T["Landroidx/car/app/model/ItemList$Builder;"],  # new-instance v0, ItemList$Builder
        0x1070, 9, 0x0000,        # invoke-direct {v0}, ItemList$Builder.<init> (m9)
    ]
    insns += row("Play Music") + row("Browse Library")
    insns += [
        0x106E, 11, 0x0000, 0x000C,  # v0 = il.build() (m11); move-result-object v0 (ItemList)
        0x0122, T["Landroidx/car/app/model/ListTemplate$Builder;"],  # new-instance v1, LT$Builder
        0x1070, 15, 0x0001,       # invoke-direct {v1}, ListTemplate$Builder.<init> (m15)
        0x021A, S["Media"],       # const-string v2, "Media"
        0x206E, 16, 0x0021, 0x010C,  # invoke-virtual {v1,v2}, setTitle (m16); move-result v1
        0x206E, 17, 0x0001, 0x010C,  # invoke-virtual {v1,v0}, setSingleList (m17); move-result v1
        0x106E, 18, 0x0001, 0x000C,  # invoke-virtual {v1}, build (m18); move-result-object v0
        0x0011,                    # return-object v0
    ]
    data.extend(code_item(5, 1, 2, insns))

    # class_data for the three app classes: 1 direct (<init>), 1 virtual (override).
    def class_data(init_idx, init_off, virt_idx, virt_off):
        cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
        cd += uleb128(init_idx) + uleb128(0x1) + uleb128(init_off)  # direct <init>, public
        cd += uleb128(virt_idx) + uleb128(0x1) + uleb128(virt_off)  # virtual override, public
        return cd

    cd_app_off = off_data + len(data)
    data.extend(class_data(0, code_app_init, 1, code_app_session))
    cd_sess_off = off_data + len(data)
    data.extend(class_data(3, code_sess_init, 4, code_sess_screen))
    cd_screen_off = off_data + len(data)
    data.extend(class_data(6, code_screen_init, 7, code_screen_tmpl))

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, N_PROTOS, off_proto_ids),
        (0x0005, N_METHODS, off_method_ids), (0x0006, N_CLASSDEFS, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 3, tl_charseq),
        (0x2001, 6, code_app_init), (0x2000, N_CLASSDEFS, cd_app_off),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)
    data.extend(align4(map_body))
    data_size = len(data)

    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)

    # protos P0..P10 (shorty, return type, params type_list offset)
    proto_ids = b""
    proto_ids += struct.pack("<III", S["V"], T["V"], 0)                    # P0 ()V
    proto_ids += struct.pack("<III", S["L"], T[TYPES[4]], 0)               # P1 ()Session
    proto_ids += struct.pack("<III", S["L"], T[TYPES[5]], 0)               # P2 ()Screen
    proto_ids += struct.pack("<III", S["L"], T[TYPES[6]], 0)               # P3 ()Template
    proto_ids += struct.pack("<III", S["LL"], T[TYPES[8]], tl_charseq)     # P4 (CharSequence)LT$B
    proto_ids += struct.pack("<III", S["LL"], T[TYPES[8]], tl_itemlist)    # P5 (ItemList)LT$B
    proto_ids += struct.pack("<III", S["L"], T[TYPES[7]], 0)               # P6 ()ListTemplate
    proto_ids += struct.pack("<III", S["LL"], T[TYPES[10]], tl_row)        # P7 (Row)IL$B
    proto_ids += struct.pack("<III", S["L"], T[TYPES[9]], 0)               # P8 ()ItemList
    proto_ids += struct.pack("<III", S["LL"], T[TYPES[12]], tl_charseq)    # P9 (CharSequence)Row$B
    proto_ids += struct.pack("<III", S["L"], T[TYPES[11]], 0)              # P10 ()Row

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m(TYPES[0], 0, "<init>"),            # m0 CarApp.<init>
        m(TYPES[0], 1, "onCreateSession"),   # m1
        m(TYPES[3], 0, "<init>"),            # m2 CarAppService.<init> (super)
        m(TYPES[1], 0, "<init>"),            # m3 CarSession.<init>
        m(TYPES[1], 2, "onCreateScreen"),    # m4
        m(TYPES[4], 0, "<init>"),            # m5 Session.<init> (super)
        m(TYPES[2], 0, "<init>"),            # m6 CarScreen.<init>
        m(TYPES[2], 3, "onGetTemplate"),     # m7
        m(TYPES[5], 0, "<init>"),            # m8 Screen.<init> (super)
        m(TYPES[10], 0, "<init>"),           # m9 ItemList$Builder.<init>
        m(TYPES[10], 7, "addItem"),          # m10
        m(TYPES[10], 8, "build"),            # m11 ItemList$Builder.build
        m(TYPES[12], 0, "<init>"),           # m12 Row$Builder.<init>
        m(TYPES[12], 9, "setTitle"),         # m13
        m(TYPES[12], 10, "build"),           # m14 Row$Builder.build
        m(TYPES[8], 0, "<init>"),            # m15 ListTemplate$Builder.<init>
        m(TYPES[8], 4, "setTitle"),          # m16
        m(TYPES[8], 5, "setSingleList"),     # m17
        m(TYPES[8], 6, "build"),             # m18 ListTemplate$Builder.build
    ])

    def class_def(cls, super_cls, cd_off):
        return struct.pack("<IIIIIIII",
                           T[cls], 0x1, T[super_cls], 0, 0xFFFFFFFF, 0, cd_off, 0)
    class_defs = class_def(TYPES[0], TYPES[3], cd_app_off)      # CarApp : CarAppService
    class_defs += class_def(TYPES[1], TYPES[4], cd_sess_off)    # CarSession : Session
    class_defs += class_def(TYPES[2], TYPES[5], cd_screen_off)  # CarScreen : Screen

    header = bytearray(header_size)
    header[0:8] = b"dex\n035\x00"
    struct.pack_into("<I", header, 0x24, header_size)
    struct.pack_into("<I", header, 0x28, 0x12345678)
    struct.pack_into("<I", header, 0x34, map_off)
    struct.pack_into("<I", header, 0x38, n_strings)
    struct.pack_into("<I", header, 0x3C, off_string_ids)
    struct.pack_into("<I", header, 0x40, n_types)
    struct.pack_into("<I", header, 0x44, off_type_ids)
    struct.pack_into("<I", header, 0x48, N_PROTOS)
    struct.pack_into("<I", header, 0x4C, off_proto_ids)
    struct.pack_into("<I", header, 0x50, 0)
    struct.pack_into("<I", header, 0x54, 0)
    struct.pack_into("<I", header, 0x58, N_METHODS)
    struct.pack_into("<I", header, 0x5C, off_method_ids)
    struct.pack_into("<I", header, 0x60, N_CLASSDEFS)
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
        sys.stderr.write("usage: make_cal_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.acl.hello",
            launcher="com.acl.hello.CarApp"),
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

# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK that draws (Phase 2, gfx/Canvas):
#   package com.acl.hello;
#   class MyView extends android.view.View {
#     protected void onDraw(Canvas c) {
#       c.drawColor(0xFF0000FF);                 // blue fill
#       Paint p = new Paint(); p.setColor(0xFFFF0000);   // red
#       c.drawRect(10f, 10f, 50f, 50f, p);       // red rect
#     }
#   }
#   public class GfxApp {
#     public static void main(String[] a) {
#       MyView v = new MyView();
#       Bitmap b = Bitmap.createBitmap(64, 64);
#       Canvas c = new Canvas(b);
#       v.draw(c);               // -> onDraw(c) -> gfx raster
#       b.aclDescribe();      // logs sample pixels
#     }
#   }
# `acl run gfx.apk --framework=<shims>` runs main() (GfxApp is not an Activity ->
# launcher falls back to static main); Canvas/Bitmap natives raster into acl::gfx.
# Exercises invoke-virtual/range + float args + an int/ref-reused register.
#
# Usage: make_gfx_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

GFXAPP = "Lcom/acl/hello/GfxApp;"
MYVIEW = "Lcom/acl/hello/MyView;"
VIEW = "Landroid/view/View;"
BITMAP = "Landroid/graphics/Bitmap;"
CANVAS = "Landroid/graphics/Canvas;"
PAINT = "Landroid/graphics/Paint;"
STRINGS = [
    GFXAPP,                     # 0
    "Ljava/lang/Object;",       # 1
    MYVIEW,                     # 2
    VIEW,                       # 3
    BITMAP,                     # 4
    CANVAS,                     # 5
    PAINT,                      # 6
    "[Ljava/lang/String;",      # 7
    "V",                        # 8
    "I",                        # 9
    "F",                        # 10
    "main",                     # 11
    "<init>",                   # 12
    "createBitmap",             # 13
    "draw",                     # 14
    "aclDescribe",           # 15
    "drawColor",                # 16
    "setColor",                 # 17
    "drawRect",                 # 18
    "onDraw",                   # 19
    "VL",                       # 20 (Object)->void
    "LII",                      # 21 createBitmap (int,int)->Bitmap
    "VI",                       # 22 (int)->void
    "VFFFFL",                   # 23 drawRect (float x4, Paint)->void
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = [GFXAPP, "Ljava/lang/Object;", MYVIEW, VIEW, BITMAP, CANVAS, PAINT,
         "[Ljava/lang/String;", "V", "I", "F"]
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
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 7, 0, 12, 2

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
    tl_bitmap = type_list([BITMAP])
    tl_canvas = type_list([CANVAS])
    tl_int = type_list(["I"])
    tl_drawrect = type_list(["F", "F", "F", "F", PAINT])

    # GfxApp.main
    code_main = off_data + len(data)
    data.extend(code_item(4, 1, 2, [
        0x0022, T[MYVIEW],        # new-instance v0, MyView
        0x1070, 0x0002, 0x0000,   # invoke-direct {v0}, MyView.<init> (m2)
        0x0113, 0x0040,           # const/16 v1, 64
        0x0213, 0x0040,           # const/16 v2, 64
        0x2071, 0x0003, 0x0021,   # invoke-static {v1,v2}, Bitmap.createBitmap (m3)
        0x010C,                   # move-result-object v1  (bitmap)
        0x0222, T[CANVAS],        # new-instance v2, Canvas
        0x2070, 0x0004, 0x0012,   # invoke-direct {v2,v1}, Canvas.<init>(Bitmap) (m4)
        0x206E, 0x0005, 0x0020,   # invoke-virtual {v0,v2}, View.draw(Canvas) (m5)
        0x106E, 0x0006, 0x0001,   # invoke-virtual {v1}, Bitmap.aclDescribe (m6)
        0x000E,                   # return-void
    ]))
    # MyView.<init>
    code_view_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0007, 0x0000,   # invoke-direct {v0}, View.<init> (m7)
        0x000E,
    ]))
    # MyView.onDraw(Canvas)  (this=v6, canvas=v7)
    code_ondraw = off_data + len(data)
    data.extend(code_item(8, 2, 6, [
        0x0014, 0x00FF, 0xFF00,   # const v0, 0xFF0000FF (blue)
        0x206E, 0x0008, 0x0007,   # invoke-virtual {v7,v0}, drawColor (m8)
        0x0522, T[PAINT],         # new-instance v5, Paint
        0x1070, 0x0009, 0x0005,   # invoke-direct {v5}, Paint.<init> (m9)
        0x0014, 0x0000, 0xFFFF,   # const v0, 0xFFFF0000 (red)
        0x206E, 0x000A, 0x0005,   # invoke-virtual {v5,v0}, setColor (m10)
        0x7007,                   # move-object v0, v7  (canvas)
        0x0114, 0x0000, 0x4120,   # const v1, 10.0f
        0x0214, 0x0000, 0x4120,   # const v2, 10.0f
        0x0314, 0x0000, 0x4248,   # const v3, 50.0f
        0x0414, 0x0000, 0x4248,   # const v4, 50.0f
        0x0674, 0x000B, 0x0000,   # invoke-virtual/range {v0..v5}, drawRect (m11)
        0x000E,                   # return-void
    ]))

    # class_data for GfxApp: 1 direct (main)
    cd_gfx = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(0))
    cd += uleb128(0) + uleb128(0x9) + uleb128(code_main)  # m0 main, public static
    data.extend(cd)
    # class_data for MyView: 1 direct (<init> m2), 1 virtual (onDraw m1)
    cd_view = off_data + len(data)
    cd2 = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd2 += uleb128(2) + uleb128(0x1) + uleb128(code_view_init)  # m2 <init>, public
    cd2 += uleb128(1) + uleb128(0x4) + uleb128(code_ondraw)     # m1 onDraw, protected
    data.extend(cd2)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids), (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 6, tl_argv),
        (0x2001, 3, code_main), (0x2000, 2, cd_gfx),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)
    data.extend(align4(map_body))
    data_size = len(data)

    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)
    proto_ids = b""
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_argv)         # 0 main
    proto_ids += struct.pack("<III", S["V"], T["V"], 0)               # 1 ()V
    proto_ids += struct.pack("<III", S["LII"], T[BITMAP], tl_ii)      # 2 createBitmap
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_bitmap)     # 3 Canvas.<init>(Bitmap)
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_canvas)    # 4 (Canvas)V draw/onDraw
    proto_ids += struct.pack("<III", S["VI"], T["V"], tl_int)       # 5 (int)V drawColor/setColor
    proto_ids += struct.pack("<III", S["VFFFFL"], T["V"], tl_drawrect)  # 6 drawRect
    # (proto for aclDescribe/<init>/View.<init> is proto 1 ()V)

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m(GFXAPP, 0, "main"),           # m0
        m(MYVIEW, 4, "onDraw"),         # m1 (Canvas)V
        m(MYVIEW, 1, "<init>"),         # m2 ()V
        m(BITMAP, 2, "createBitmap"),   # m3
        m(CANVAS, 3, "<init>"),         # m4 (Bitmap)V
        m(VIEW, 4, "draw"),             # m5 (Canvas)V
        m(BITMAP, 1, "aclDescribe"), # m6 ()V
        m(VIEW, 1, "<init>"),           # m7 ()V
        m(CANVAS, 5, "drawColor"),      # m8 (int)V
        m(PAINT, 1, "<init>"),          # m9 ()V
        m(PAINT, 5, "setColor"),        # m10 (int)V
        m(CANVAS, 6, "drawRect"),       # m11
    ])
    class_defs = struct.pack("<IIIIIIII",
                             T[GFXAPP], 0x1, T["Ljava/lang/Object;"], 0,
                             0xFFFFFFFF, 0, cd_gfx, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[MYVIEW], 0x1, T[VIEW], 0, 0xFFFFFFFF, 0, cd_view, 0)

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
        sys.stderr.write("usage: make_gfx_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.acl.hello",
            launcher="com.acl.hello.GfxApp"),
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

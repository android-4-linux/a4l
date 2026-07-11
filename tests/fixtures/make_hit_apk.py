# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK that proves ViewGroup touch hit-testing (Phase 2 input routing):
#   package com.aclcxx.hello;
#   class Tile extends android.view.View {
#     int color;
#     Tile(Context c, int color) { super(c); this.color = color; }
#     protected void onDraw(Canvas c) { c.drawColor(color); }
#     public boolean onTouchEvent(MotionEvent e) {
#       Log.i("aclcxx.hit", getTop() == 0 ? "top" : "bottom");   // which child got it
#       return true;
#     }
#   }
#   class Panel extends android.view.ViewGroup {
#     Panel(Context c) { super(c); addView(new Tile(c, BLUE)); addView(new Tile(c, RED)); }
#     protected void onLayout(...) { getChildAt(0).layout(0,0,w,h/2); getChildAt(1).layout(0,h/2,w,h); }
#   }
#   class MainActivity extends android.app.Activity {
#     protected void onCreate(Bundle b) { super.onCreate(b); setContentView(new Panel(this)); }
#   }
# The compositor taps at (100,50) — inside the TOP tile. ViewGroup.dispatchTouchEvent
# must route it to that child (offsetting coords), so it logs "top", not "bottom".
#
# Usage: make_hit_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

MAINACT = "Lcom/aclcxx/hello/MainActivity;"
PANEL = "Lcom/aclcxx/hello/Panel;"
TILE = "Lcom/aclcxx/hello/Tile;"
ACTIVITY = "Landroid/app/Activity;"
VIEWGROUP = "Landroid/view/ViewGroup;"
VIEW = "Landroid/view/View;"
CONTEXT = "Landroid/content/Context;"
BUNDLE = "Landroid/os/Bundle;"
CANVAS = "Landroid/graphics/Canvas;"
MOTIONEVENT = "Landroid/view/MotionEvent;"
LOG = "Landroid/util/Log;"
STRING = "Ljava/lang/String;"

STRINGS = [
    MAINACT, PANEL, TILE, ACTIVITY, VIEWGROUP, VIEW, CONTEXT, BUNDLE, CANVAS,
    MOTIONEVENT, LOG, STRING,
    "V", "I", "Z",
    "VL", "VZIIII", "LI", "VIIII", "VLI", "VI", "ZL", "ILL",
    "<init>", "onCreate", "setContentView", "onLayout", "addView", "getChildAt",
    "layout", "getTop", "onDraw", "onTouchEvent", "i", "drawColor", "color",
    "aclcxx.hit", "top", "bottom",
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = [MAINACT, PANEL, TILE, ACTIVITY, VIEWGROUP, VIEW, CONTEXT, BUNDLE, CANVAS,
         MOTIONEVENT, LOG, STRING, "V", "I", "Z"]
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
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 13, 1, 18, 3

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

    tl_bundle = type_list([BUNDLE])
    first_tl = tl_bundle
    tl_view = type_list([VIEW])
    tl_ctx = type_list([CONTEXT])
    tl_onlayout = type_list(["Z", "I", "I", "I", "I"])
    tl_i = type_list(["I"])
    tl_iiii = type_list(["I", "I", "I", "I"])
    tl_ctxi = type_list([CONTEXT, "I"])
    tl_canvas = type_list([CANVAS])
    tl_motion = type_list([MOTIONEVENT])
    tl_strstr = type_list([STRING, STRING])

    # --- code items ---
    code_mainact_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0002, 0x0000, 0x000E,   # invoke-direct {v0}, Activity.<init> (m2); return-void
    ]))
    first_code = code_mainact_init
    code_mainact_oncreate = off_data + len(data)
    data.extend(code_item(4, 2, 2, [
        0x206F, 0x0003, 0x0032,   # invoke-super {v2,v3}, Activity.onCreate (m3)
        0x0022, 0x0001,           # new-instance v0, Panel
        0x2070, 0x0005, 0x0020,   # invoke-direct {v0,v2}, Panel.<init>(Context) (m5)
        0x206E, 0x0004, 0x0002,   # invoke-virtual {v2,v0}, setContentView (m4)
        0x000E,
    ]))
    code_panel_init = off_data + len(data)
    data.extend(code_item(5, 2, 3, [
        0x2070, 0x0007, 0x0043,   # invoke-direct {v3,v4}, ViewGroup.<init>(Context) (m7)
        0x0022, 0x0002,           # new-instance v0, Tile
        0x0114, 0x00FF, 0xFF00,   # const v1, 0xFF0000FF (blue)
        0x3070, 0x000B, 0x0140,   # invoke-direct {v0,v4,v1}, Tile.<init>(Context,int) (m11)
        0x206E, 0x0008, 0x0003,   # invoke-virtual {v3,v0}, addView (m8)
        0x0022, 0x0002,           # new-instance v0, Tile
        0x0114, 0x0000, 0xFFFF,   # const v1, 0xFFFF0000 (red)
        0x3070, 0x000B, 0x0140,   # invoke-direct {v0,v4,v1}, Tile.<init> (m11)
        0x206E, 0x0008, 0x0003,   # invoke-virtual {v3,v0}, addView (m8)
        0x000E,
    ]))
    code_panel_onlayout = off_data + len(data)
    data.extend(code_item(11, 6, 5, [
        0x0091, 0x0709,           # sub-int v0, v9, v7   (w)
        0x0191, 0x080A,           # sub-int v1, v10, v8  (h)
        0x02DB, 0x0201,           # div-int/lit8 v2, v1, 2  (half)
        0x0412,                   # const/4 v4, 0
        0x0312,                   # const/4 v3, 0
        0x206E, 0x0009, 0x0035,   # invoke-virtual {v5,v3}, getChildAt (m9)
        0x030C,                   # move-result-object v3  (child0)
        0x526E, 0x000A, 0x0443,   # invoke-virtual {v3,v4,v4,v0,v2}, layout(0,0,w,half) (m10)
        0x1312,                   # const/4 v3, 1
        0x206E, 0x0009, 0x0035,   # invoke-virtual {v5,v3}, getChildAt (m9)
        0x030C,                   # move-result-object v3  (child1)
        0x516E, 0x000A, 0x0243,   # invoke-virtual {v3,v4,v2,v0,v1}, layout(0,half,w,h) (m10)
        0x000E,
    ]))
    code_tile_init = off_data + len(data)
    data.extend(code_item(3, 3, 2, [
        0x2070, 0x000C, 0x0010,   # invoke-direct {v0,v1}, View.<init>(Context) (m12)
        0x0259, 0x0000,           # iput v2, v0, Tile.color (f0)
        0x000E,
    ]))
    code_tile_ondraw = off_data + len(data)
    data.extend(code_item(3, 2, 2, [
        0x1052, 0x0000,           # iget v0, v1, Tile.color (f0)
        0x206E, 0x000D, 0x0002,   # invoke-virtual {v2,v0}, drawColor (m13)
        0x000E,
    ]))
    code_tile_ontouch = off_data + len(data)
    data.extend(code_item(5, 2, 2, [
        0x106E, 0x000E, 0x0003,   # invoke-virtual {v3}, getTop (m14)
        0x000A,                   # move-result v0
        0x011A, S["aclcxx.hit"],  # const-string v1, "aclcxx.hit"
        0x0038, 0x0005,           # if-eqz v0, :top (+5)   (getTop()==0 -> the top tile)
        0x021A, S["bottom"],      # const-string v2, "bottom"  (getTop() != 0)
        0x0328,                   # goto :log (+3)
        0x021A, S["top"],         # :top const-string v2, "top"
        0x2071, 0x000F, 0x0021,   # :log invoke-static {v1,v2}, Log.i (m15)
        0x1012,                   # const/4 v0, 1
        0x000F,                   # return v0  (true)
    ]))

    # --- class_data ---
    cd_mainact = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(0) + uleb128(0x10001) + uleb128(code_mainact_init)     # m0 <init>
    cd += uleb128(1) + uleb128(0x4) + uleb128(code_mainact_oncreate)     # m1 onCreate
    data.extend(cd)
    cd_panel = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(5) + uleb128(0x10001) + uleb128(code_panel_init)       # m5 <init>
    cd += uleb128(6) + uleb128(0x4) + uleb128(code_panel_onlayout)       # m6 onLayout
    data.extend(cd)
    cd_tile = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(1) + uleb128(1) + uleb128(2))
    cd += uleb128(0) + uleb128(0x2)                                      # f0 color (private)
    cd += uleb128(11) + uleb128(0x10001) + uleb128(code_tile_init)       # m11 <init> (direct)
    cd += uleb128(16) + uleb128(0x4) + uleb128(code_tile_ondraw)         # m16 onDraw (virtual: diff 16 from 0)
    cd += uleb128(1) + uleb128(0x1) + uleb128(code_tile_ontouch)         # m17 onTouchEvent (diff 1, public)
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0004, n_fields, off_field_ids), (0x0005, n_methods, off_method_ids),
        (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 10, first_tl),
        (0x2001, 7, first_code), (0x2000, 3, cd_mainact),
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
    proto_ids += struct.pack("<III", S["V"], T["V"], 0)                  # 0 ()V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_bundle)         # 1 (Bundle)V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_view)           # 2 (View)V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_ctx)            # 3 (Context)V
    proto_ids += struct.pack("<III", S["VZIIII"], T["V"], tl_onlayout)   # 4 onLayout
    proto_ids += struct.pack("<III", S["LI"], T[VIEW], tl_i)             # 5 (int)View
    proto_ids += struct.pack("<III", S["VIIII"], T["V"], tl_iiii)        # 6 layout
    proto_ids += struct.pack("<III", S["VLI"], T["V"], tl_ctxi)          # 7 Tile.<init>
    proto_ids += struct.pack("<III", S["VI"], T["V"], tl_i)             # 8 (int)V
    proto_ids += struct.pack("<III", S["I"], T["I"], 0)                  # 9 ()I getTop
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_canvas)         # 10 (Canvas)V onDraw
    proto_ids += struct.pack("<III", S["ZL"], T["Z"], tl_motion)         # 11 (MotionEvent)Z
    proto_ids += struct.pack("<III", S["ILL"], T["I"], tl_strstr)        # 12 (String,String)I

    field_ids = struct.pack("<HHI", T[TILE], T["I"], S["color"])         # f0 Tile.color:I

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m(MAINACT, 0, "<init>"),           # m0
        m(MAINACT, 1, "onCreate"),         # m1
        m(ACTIVITY, 0, "<init>"),          # m2
        m(ACTIVITY, 1, "onCreate"),        # m3
        m(ACTIVITY, 2, "setContentView"),  # m4
        m(PANEL, 3, "<init>"),             # m5
        m(PANEL, 4, "onLayout"),           # m6
        m(VIEWGROUP, 3, "<init>"),         # m7
        m(VIEWGROUP, 2, "addView"),        # m8
        m(VIEWGROUP, 5, "getChildAt"),     # m9
        m(VIEW, 6, "layout"),              # m10
        m(TILE, 7, "<init>"),              # m11
        m(VIEW, 3, "<init>"),              # m12
        m(CANVAS, 8, "drawColor"),         # m13
        m(VIEW, 9, "getTop"),              # m14
        m(LOG, 12, "i"),                   # m15
        m(TILE, 10, "onDraw"),             # m16
        m(TILE, 11, "onTouchEvent"),       # m17
    ])
    class_defs = b""
    class_defs += struct.pack("<IIIIIIII",
                              T[MAINACT], 0x1, T[ACTIVITY], 0, 0xFFFFFFFF, 0, cd_mainact, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[PANEL], 0x1, T[VIEWGROUP], 0, 0xFFFFFFFF, 0, cd_panel, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[TILE], 0x1, T[VIEW], 0, 0xFFFFFFFF, 0, cd_tile, 0)

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
    struct.pack_into("<I", header, 0x54, off_field_ids)
    struct.pack_into("<I", header, 0x58, n_methods)
    struct.pack_into("<I", header, 0x5C, off_method_ids)
    struct.pack_into("<I", header, 0x60, n_classdefs)
    struct.pack_into("<I", header, 0x64, off_class_defs)
    struct.pack_into("<I", header, 0x68, data_size)
    struct.pack_into("<I", header, 0x6C, off_data)

    dex = bytearray(header) + string_ids + type_ids + proto_ids + field_ids + \
        method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))
    dex[12:32] = hashlib.sha1(bytes(dex[32:])).digest()
    struct.pack_into("<I", dex, 0x08, zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF)
    return bytes(dex)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_hit_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.aclcxx.hello",
            launcher="com.aclcxx.hello.MainActivity"),
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

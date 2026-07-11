# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK that ANIMATES via android.view.Choreographer (Phase 2, continuous
# presentation):
#   package com.aclcxx.hello;
#   class AnimView extends android.view.View implements Choreographer.FrameCallback {
#     int frame;
#     AnimView(Context c) { super(c); }
#     public void doFrame(long t) {          // one vsync tick
#       frame++;
#       invalidate();
#       Choreographer.getInstance().postFrameCallback(this);   // re-post (keep animating)
#     }
#     protected void onDraw(Canvas c) {
#       c.drawColor(0xFF0000FF);             // blue field
#       Paint p = new Paint(); p.setColor(0xFFFF0000);
#       int y = frame * 8;
#       c.drawRect(0, y, getWidth(), y + 24, p);   // red band sweeping down
#     }
#   }
#   class MainActivity extends android.app.Activity {
#     protected void onCreate(Bundle b) {
#       super.onCreate(b);
#       AnimView v = new AnimView(this);
#       setContentView(v);
#       Choreographer.getInstance().postFrameCallback(v);   // start the loop
#     }
#   }
# `aclcxx run anim.apk --present-frames=N` drives the ViewRoot/Choreographer loop:
# each Wayland frame tick fires doFrame (frame++/re-post) and re-draws, so the red
# band moves and N distinct frames are presented.
#
# Usage: make_anim_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

MAINACT = "Lcom/aclcxx/hello/MainActivity;"
ANIMVIEW = "Lcom/aclcxx/hello/AnimView;"
ACTIVITY = "Landroid/app/Activity;"
VIEW = "Landroid/view/View;"
CONTEXT = "Landroid/content/Context;"
BUNDLE = "Landroid/os/Bundle;"
CANVAS = "Landroid/graphics/Canvas;"
PAINT = "Landroid/graphics/Paint;"
CHOREO = "Landroid/view/Choreographer;"
FRAMECB = "Landroid/view/Choreographer$FrameCallback;"

STRINGS = [
    MAINACT, ANIMVIEW, ACTIVITY, VIEW, CONTEXT, BUNDLE, CANVAS, PAINT, CHOREO, FRAMECB,
    "V", "I", "J", "F",
    "VL", "VJ", "L", "VI", "VFFFFL",
    "<init>", "onCreate", "setContentView", "doFrame", "onDraw", "invalidate",
    "getWidth", "getInstance", "postFrameCallback", "setColor", "drawColor",
    "drawRect", "frame",
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = [MAINACT, ANIMVIEW, ACTIVITY, VIEW, CONTEXT, BUNDLE, CANVAS, PAINT, CHOREO,
         FRAMECB, "V", "I", "J", "F"]
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
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 11, 1, 17, 2

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
    tl_j = type_list(["J"])
    tl_canvas = type_list([CANVAS])
    tl_framecb = type_list([FRAMECB])   # also the AnimView interfaces list
    tl_i = type_list(["I"])
    tl_drawrect = type_list(["F", "F", "F", "F", PAINT])

    # --- code items ---
    code_mainact_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0002, 0x0000,   # invoke-direct {v0}, Activity.<init> (m2)
        0x000E,
    ]))
    first_code = code_mainact_init
    code_mainact_oncreate = off_data + len(data)
    data.extend(code_item(5, 2, 2, [
        0x206F, 0x0003, 0x0043,   # invoke-super {v3,v4}, Activity.onCreate (m3)
        0x0022, 0x0001,           # new-instance v0, AnimView
        0x2070, 0x0005, 0x0030,   # invoke-direct {v0,v3}, AnimView.<init>(Context) (m5)
        0x206E, 0x0004, 0x0003,   # invoke-virtual {v3,v0}, setContentView (m4)
        0x0071, 0x000B, 0x0000,   # invoke-static {}, Choreographer.getInstance (m11)
        0x010C,                   # move-result-object v1
        0x206E, 0x000C, 0x0001,   # invoke-virtual {v1,v0}, postFrameCallback (m12)
        0x000E,                   # return-void
    ]))
    code_animview_init = off_data + len(data)
    data.extend(code_item(2, 2, 2, [
        0x2070, 0x0008, 0x0010,   # invoke-direct {v0,v1}, View.<init>(Context) (m8)
        0x000E,
    ]))
    code_animview_doframe = off_data + len(data)
    data.extend(code_item(5, 3, 2, [
        0x2052, 0x0000,           # iget v0, v2, AnimView.frame (f0)
        0x00D8, 0x0100,           # add-int/lit8 v0, v0, 1
        0x2059, 0x0000,           # iput v0, v2, AnimView.frame (f0)
        0x106E, 0x0009, 0x0002,   # invoke-virtual {v2}, View.invalidate (m9)
        0x0071, 0x000B, 0x0000,   # invoke-static {}, Choreographer.getInstance (m11)
        0x010C,                   # move-result-object v1
        0x206E, 0x000C, 0x0021,   # invoke-virtual {v1,v2}, postFrameCallback(this) (m12)
        0x000E,                   # return-void
    ]))
    code_animview_ondraw = off_data + len(data)
    data.extend(code_item(12, 2, 6, [
        0x0614, 0x00FF, 0xFF00,   # const v6, 0xFF0000FF (blue)
        0x206E, 0x000F, 0x006B,   # invoke-virtual {v11,v6}, drawColor (m15)
        0x0522, 0x0007,           # new-instance v5, Paint
        0x1070, 0x000D, 0x0005,   # invoke-direct {v5}, Paint.<init> (m13)
        0x0614, 0x0000, 0xFFFF,   # const v6, 0xFFFF0000 (red)
        0x206E, 0x000E, 0x0065,   # invoke-virtual {v5,v6}, setColor (m14)
        0xB007,                   # move-object v0, v11 (canvas)
        0x0114, 0x0000, 0x0000,   # const v1, 0.0f
        0xA652, 0x0000,           # iget v6, v10, frame (f0)
        0x07DA, 0x0806,           # mul-int/lit8 v7, v6, 8   (y)
        0x7282,                   # int-to-float v2, v7      (yf)
        0x08D8, 0x1807,           # add-int/lit8 v8, v7, 24  (yb)
        0x8482,                   # int-to-float v4, v8      (ybf)
        0x106E, 0x000A, 0x000A,   # invoke-virtual {v10}, getWidth (m10)
        0x060A,                   # move-result v6
        0x6382,                   # int-to-float v3, v6      (wf)
        0x0674, 0x0010, 0x0000,   # invoke-virtual/range {v0..v5}, drawRect (m16)
        0x000E,                   # return-void
    ]))

    # --- class_data ---
    cd_mainact = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(0) + uleb128(0x10001) + uleb128(code_mainact_init)     # m0 <init>
    cd += uleb128(1) + uleb128(0x4) + uleb128(code_mainact_oncreate)     # m1 onCreate
    data.extend(cd)
    cd_animview = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(1) + uleb128(1) + uleb128(2))
    cd += uleb128(0) + uleb128(0x2)                                      # f0 frame (private)
    cd += uleb128(5) + uleb128(0x10001) + uleb128(code_animview_init)    # m5 <init>
    cd += uleb128(6) + uleb128(0x1) + uleb128(code_animview_doframe)     # m6 doFrame (public: iface)
    cd += uleb128(1) + uleb128(0x4) + uleb128(code_animview_ondraw)      # m7 onDraw (diff 1)
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0004, n_fields, off_field_ids), (0x0005, n_methods, off_method_ids),
        (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 8, first_tl),
        (0x2001, 5, first_code), (0x2000, 2, cd_mainact),
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
    proto_ids += struct.pack("<III", S["VJ"], T["V"], tl_j)              # 4 (long)V doFrame
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_canvas)         # 5 (Canvas)V onDraw
    proto_ids += struct.pack("<III", S["I"], T["I"], 0)                  # 6 ()I getWidth
    proto_ids += struct.pack("<III", S["L"], T[CHOREO], 0)               # 7 ()Choreographer
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_framecb)        # 8 (FrameCallback)V
    proto_ids += struct.pack("<III", S["VI"], T["V"], tl_i)              # 9 (int)V
    proto_ids += struct.pack("<III", S["VFFFFL"], T["V"], tl_drawrect)   # 10 drawRect

    field_ids = struct.pack("<HHI", T[ANIMVIEW], T["I"], S["frame"])     # f0 AnimView.frame:I

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m(MAINACT, 0, "<init>"),           # m0
        m(MAINACT, 1, "onCreate"),         # m1
        m(ACTIVITY, 0, "<init>"),          # m2
        m(ACTIVITY, 1, "onCreate"),        # m3
        m(ACTIVITY, 2, "setContentView"),  # m4
        m(ANIMVIEW, 3, "<init>"),          # m5
        m(ANIMVIEW, 4, "doFrame"),         # m6
        m(ANIMVIEW, 5, "onDraw"),          # m7
        m(VIEW, 3, "<init>"),              # m8
        m(VIEW, 0, "invalidate"),          # m9
        m(VIEW, 6, "getWidth"),            # m10
        m(CHOREO, 7, "getInstance"),       # m11
        m(CHOREO, 8, "postFrameCallback"), # m12
        m(PAINT, 0, "<init>"),             # m13
        m(PAINT, 9, "setColor"),           # m14
        m(CANVAS, 9, "drawColor"),         # m15
        m(CANVAS, 10, "drawRect"),         # m16
    ])
    class_defs = b""
    class_defs += struct.pack("<IIIIIIII",
                              T[MAINACT], 0x1, T[ACTIVITY], 0, 0xFFFFFFFF, 0, cd_mainact, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[ANIMVIEW], 0x1, T[VIEW], tl_framecb, 0xFFFFFFFF, 0, cd_animview, 0)

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
        sys.stderr.write("usage: make_anim_apk.py <output.apk>\n")
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

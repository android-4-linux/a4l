# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK that reacts to keyboard input (Phase 2, wl_keyboard -> KeyEvent):
#   package com.acl.hello;
#   class KeyView extends android.view.View {
#     KeyView(Context c) { super(c); }
#     public boolean onKeyDown(int keyCode, KeyEvent e) {
#       Log.i("acl.key", "onKeyDown");   // proof the key arrived
#       return true;
#     }
#     protected void onDraw(Canvas c) { c.drawColor(0xFF0000FF); }
#   }
#   class MainActivity extends android.app.Activity {
#     protected void onCreate(Bundle b) { super.onCreate(b); setContentView(new KeyView(this)); }
#   }
# `acl run key.apk --present-frames=0` presents until close; the compositor
# injects a key press, the ViewRoot driver turns it into a KeyEvent and dispatches
# it (View.dispatchKeyEvent -> onKeyDown), which logs.
#
# Usage: make_key_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

MAINACT = "Lcom/acl/hello/MainActivity;"
KEYVIEW = "Lcom/acl/hello/KeyView;"
ACTIVITY = "Landroid/app/Activity;"
VIEW = "Landroid/view/View;"
CONTEXT = "Landroid/content/Context;"
BUNDLE = "Landroid/os/Bundle;"
CANVAS = "Landroid/graphics/Canvas;"
KEYEVENT = "Landroid/view/KeyEvent;"
LOG = "Landroid/util/Log;"
STRING = "Ljava/lang/String;"

STRINGS = [
    MAINACT, KEYVIEW, ACTIVITY, VIEW, CONTEXT, BUNDLE, CANVAS, KEYEVENT, LOG, STRING,
    "V", "I", "Z",
    "VL", "ZIL", "VI", "ILL",
    "<init>", "onCreate", "setContentView", "onKeyDown", "onDraw", "i", "drawColor",
    "acl.key",
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = [MAINACT, KEYVIEW, ACTIVITY, VIEW, CONTEXT, BUNDLE, CANVAS, KEYEVENT,
         LOG, STRING, "V", "I", "Z"]
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
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 8, 0, 11, 2

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
    tl_ikeyev = type_list(["I", KEYEVENT])
    tl_canvas = type_list([CANVAS])
    tl_strstr = type_list([STRING, STRING])
    tl_i = type_list(["I"])

    # --- code items ---
    code_mainact_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0002, 0x0000,   # invoke-direct {v0}, Activity.<init> (m2)
        0x000E,
    ]))
    first_code = code_mainact_init
    code_mainact_oncreate = off_data + len(data)
    data.extend(code_item(4, 2, 2, [
        0x206F, 0x0003, 0x0032,   # invoke-super {v2,v3}, Activity.onCreate (m3)
        0x0022, 0x0001,           # new-instance v0, KeyView
        0x2070, 0x0005, 0x0020,   # invoke-direct {v0,v2}, KeyView.<init>(Context) (m5)
        0x206E, 0x0004, 0x0002,   # invoke-virtual {v2,v0}, setContentView (m4)
        0x000E,
    ]))
    code_keyview_init = off_data + len(data)
    data.extend(code_item(2, 2, 2, [
        0x2070, 0x0008, 0x0010,   # invoke-direct {v0,v1}, View.<init>(Context) (m8)
        0x000E,
    ]))
    code_keyview_onkeydown = off_data + len(data)
    data.extend(code_item(6, 3, 2, [
        0x001A, S["acl.key"],   # const-string v0, "acl.key"
        0x011A, S["onKeyDown"],    # const-string v1, "onKeyDown"
        0x2071, 0x0009, 0x0010,    # invoke-static {v0,v1}, Log.i (m9)
        0x1212,                    # const/4 v2, 1
        0x020F,                    # return v2  (true)
    ]))
    code_keyview_ondraw = off_data + len(data)
    data.extend(code_item(3, 2, 2, [
        0x0014, 0x00FF, 0xFF00,    # const v0, 0xFF0000FF (blue)
        0x206E, 0x000A, 0x0002,    # invoke-virtual {v2,v0}, drawColor (m10)
        0x000E,
    ]))

    # --- class_data ---
    cd_mainact = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(0) + uleb128(0x10001) + uleb128(code_mainact_init)     # m0 <init>
    cd += uleb128(1) + uleb128(0x4) + uleb128(code_mainact_oncreate)     # m1 onCreate
    data.extend(cd)
    cd_keyview = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(2))
    cd += uleb128(5) + uleb128(0x10001) + uleb128(code_keyview_init)     # m5 <init>
    cd += uleb128(6) + uleb128(0x1) + uleb128(code_keyview_onkeydown)    # m6 onKeyDown (public)
    cd += uleb128(1) + uleb128(0x4) + uleb128(code_keyview_ondraw)       # m7 onDraw (diff 1)
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids), (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 7, first_tl),
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
    proto_ids += struct.pack("<III", S["ZIL"], T["Z"], tl_ikeyev)        # 4 (int,KeyEvent)Z
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_canvas)         # 5 (Canvas)V
    proto_ids += struct.pack("<III", S["ILL"], T["I"], tl_strstr)        # 6 (String,String)I
    proto_ids += struct.pack("<III", S["VI"], T["V"], tl_i)              # 7 (int)V

    field_ids = b""

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m(MAINACT, 0, "<init>"),           # m0
        m(MAINACT, 1, "onCreate"),         # m1
        m(ACTIVITY, 0, "<init>"),          # m2
        m(ACTIVITY, 1, "onCreate"),        # m3
        m(ACTIVITY, 2, "setContentView"),  # m4
        m(KEYVIEW, 3, "<init>"),           # m5
        m(KEYVIEW, 4, "onKeyDown"),        # m6
        m(KEYVIEW, 5, "onDraw"),           # m7
        m(VIEW, 3, "<init>"),              # m8
        m(LOG, 6, "i"),                    # m9
        m(CANVAS, 7, "drawColor"),         # m10
    ])
    class_defs = b""
    class_defs += struct.pack("<IIIIIIII",
                              T[MAINACT], 0x1, T[ACTIVITY], 0, 0xFFFFFFFF, 0, cd_mainact, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[KEYVIEW], 0x1, T[VIEW], 0, 0xFFFFFFFF, 0, cd_keyview, 0)

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

    dex = bytearray(header) + string_ids + type_ids + proto_ids + field_ids + \
        method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))
    dex[12:32] = hashlib.sha1(bytes(dex[32:])).digest()
    struct.pack_into("<I", dex, 0x08, zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF)
    return bytes(dex)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_key_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.acl.hello",
            launcher="com.acl.hello.MainActivity"),
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

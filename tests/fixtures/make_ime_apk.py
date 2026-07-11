# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK that receives IME-committed text (Phase 2, zwp_text_input_v3 ->
# InputConnection):
#   package com.aclcxx.hello;
#   class ImeConn implements android.view.inputmethod.InputConnection {
#     public boolean commitText(CharSequence text, int newCursorPosition) {
#       Log.i("aclcxx.ime", "commitText");   // the compositor's IME committed text
#       return true;
#     }
#   }
#   class ImeView extends android.view.View {
#     ImeView(Context c) { super(c); }
#     protected void onDraw(Canvas c) { c.drawColor(0xFF0000FF); }
#     public InputConnection onCreateInputConnection(EditorInfo o) { return new ImeConn(); }
#   }
#   class MainActivity extends android.app.Activity {
#     protected void onCreate(Bundle b) { super.onCreate(b); setContentView(new ImeView(this)); }
#   }
# `aclcxx run ime.apk --present-frames=0`: the view returns an InputConnection, so
# the driver enables Wayland text-input; the compositor's IME sends commit_string,
# which the driver routes to InputConnection.commitText -> logs "commitText".
#
# Usage: make_ime_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

MAINACT = "Lcom/aclcxx/hello/MainActivity;"
IMEVIEW = "Lcom/aclcxx/hello/ImeView;"
IMECONN = "Lcom/aclcxx/hello/ImeConn;"
ACTIVITY = "Landroid/app/Activity;"
VIEW = "Landroid/view/View;"
CONTEXT = "Landroid/content/Context;"
BUNDLE = "Landroid/os/Bundle;"
CANVAS = "Landroid/graphics/Canvas;"
OBJECT = "Ljava/lang/Object;"
INPUTCONN = "Landroid/view/inputmethod/InputConnection;"
EDITORINFO = "Landroid/view/inputmethod/EditorInfo;"
CHARSEQ = "Ljava/lang/CharSequence;"
LOG = "Landroid/util/Log;"
STRING = "Ljava/lang/String;"

STRINGS = [
    MAINACT, IMEVIEW, IMECONN, ACTIVITY, VIEW, CONTEXT, BUNDLE, CANVAS, OBJECT,
    INPUTCONN, EDITORINFO, CHARSEQ, LOG, STRING,
    "V", "I", "Z",
    "VL", "LL", "ZLI", "VI", "ILL",
    "<init>", "onCreate", "setContentView", "onDraw", "onCreateInputConnection",
    "commitText", "drawColor", "i", "aclcxx.ime",
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = [MAINACT, IMEVIEW, IMECONN, ACTIVITY, VIEW, CONTEXT, BUNDLE, CANVAS, OBJECT,
         INPUTCONN, EDITORINFO, CHARSEQ, LOG, STRING, "V", "I", "Z"]
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
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 9, 0, 14, 3

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
    tl_canvas = type_list([CANVAS])
    tl_editorinfo = type_list([EDITORINFO])
    tl_charseq_i = type_list([CHARSEQ, "I"])
    tl_i = type_list(["I"])
    tl_strstr = type_list([STRING, STRING])
    tl_inputconn = type_list([INPUTCONN])   # ImeConn's interfaces list

    # --- code items ---
    code_mainact_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [0x1070, 0x0002, 0x0000, 0x000E]))
    first_code = code_mainact_init
    code_mainact_oncreate = off_data + len(data)
    data.extend(code_item(4, 2, 2, [
        0x206F, 0x0003, 0x0032,   # invoke-super {v2,v3}, Activity.onCreate (m3)
        0x0022, 0x0001,           # new-instance v0, ImeView
        0x2070, 0x0005, 0x0020,   # invoke-direct {v0,v2}, ImeView.<init>(Context) (m5)
        0x206E, 0x0004, 0x0002,   # invoke-virtual {v2,v0}, setContentView (m4)
        0x000E,
    ]))
    code_imeview_init = off_data + len(data)
    data.extend(code_item(2, 2, 2, [
        0x2070, 0x000A, 0x0010,   # invoke-direct {v0,v1}, View.<init>(Context) (m10)
        0x000E,
    ]))
    code_imeview_ondraw = off_data + len(data)
    data.extend(code_item(3, 2, 2, [
        0x0014, 0x00FF, 0xFF00,   # const v0, 0xFF0000FF (blue)
        0x206E, 0x000C, 0x0002,   # invoke-virtual {v2,v0}, drawColor (m12)
        0x000E,
    ]))
    code_imeview_createic = off_data + len(data)
    data.extend(code_item(3, 2, 1, [
        0x0022, 0x0002,           # new-instance v0, ImeConn
        0x1070, 0x0008, 0x0000,   # invoke-direct {v0}, ImeConn.<init> (m8)
        0x0011,                   # return-object v0
    ]))
    code_imeconn_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x000B, 0x0000,   # invoke-direct {v0}, Object.<init> (m11)
        0x000E,
    ]))
    code_imeconn_commit = off_data + len(data)
    data.extend(code_item(6, 3, 2, [
        0x001A, S["aclcxx.ime"],  # const-string v0, "aclcxx.ime"
        0x011A, S["commitText"],  # const-string v1, "commitText"
        0x2071, 0x000D, 0x0010,   # invoke-static {v0,v1}, Log.i (m13)
        0x1212,                   # const/4 v2, 1
        0x020F,                   # return v2  (true)
    ]))

    # --- class_data ---
    cd_mainact = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(0) + uleb128(0x10001) + uleb128(code_mainact_init)     # m0 <init>
    cd += uleb128(1) + uleb128(0x4) + uleb128(code_mainact_oncreate)     # m1 onCreate
    data.extend(cd)
    cd_imeview = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(2))
    cd += uleb128(5) + uleb128(0x10001) + uleb128(code_imeview_init)     # m5 <init>
    cd += uleb128(6) + uleb128(0x4) + uleb128(code_imeview_ondraw)       # m6 onDraw (virtual diff 6)
    cd += uleb128(1) + uleb128(0x1) + uleb128(code_imeview_createic)     # m7 onCreateInputConnection (diff 1, public)
    data.extend(cd)
    cd_imeconn = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(8) + uleb128(0x10001) + uleb128(code_imeconn_init)     # m8 <init>
    cd += uleb128(9) + uleb128(0x1) + uleb128(code_imeconn_commit)       # m9 commitText (virtual diff 9, public)
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids), (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 9, first_tl),
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
    proto_ids += struct.pack("<III", S["V"], T["V"], 0)                   # 0 ()V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_bundle)          # 1 (Bundle)V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_view)            # 2 (View)V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_ctx)             # 3 (Context)V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_canvas)          # 4 (Canvas)V
    proto_ids += struct.pack("<III", S["LL"], T[INPUTCONN], tl_editorinfo)  # 5 (EditorInfo)InputConnection
    proto_ids += struct.pack("<III", S["ZLI"], T["Z"], tl_charseq_i)      # 6 (CharSequence,int)Z
    proto_ids += struct.pack("<III", S["VI"], T["V"], tl_i)              # 7 (int)V
    proto_ids += struct.pack("<III", S["ILL"], T["I"], tl_strstr)         # 8 (String,String)I

    field_ids = b""

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m(MAINACT, 0, "<init>"),                    # m0
        m(MAINACT, 1, "onCreate"),                  # m1
        m(ACTIVITY, 0, "<init>"),                   # m2
        m(ACTIVITY, 1, "onCreate"),                 # m3
        m(ACTIVITY, 2, "setContentView"),           # m4
        m(IMEVIEW, 3, "<init>"),                    # m5
        m(IMEVIEW, 4, "onDraw"),                    # m6
        m(IMEVIEW, 5, "onCreateInputConnection"),   # m7
        m(IMECONN, 0, "<init>"),                    # m8
        m(IMECONN, 6, "commitText"),                # m9
        m(VIEW, 3, "<init>"),                       # m10
        m(OBJECT, 0, "<init>"),                     # m11
        m(CANVAS, 7, "drawColor"),                  # m12
        m(LOG, 8, "i"),                             # m13
    ])
    class_defs = b""
    class_defs += struct.pack("<IIIIIIII",
                              T[MAINACT], 0x1, T[ACTIVITY], 0, 0xFFFFFFFF, 0, cd_mainact, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[IMEVIEW], 0x1, T[VIEW], 0, 0xFFFFFFFF, 0, cd_imeview, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[IMECONN], 0x1, T[OBJECT], tl_inputconn, 0xFFFFFFFF, 0, cd_imeconn, 0)

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
        sys.stderr.write("usage: make_ime_apk.py <output.apk>\n")
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
